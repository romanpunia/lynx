#include <vitex/vitex.h>
#include <vitex/core/network.h>
#include <vitex/core/engine.h>
#include <vitex/network/http.h>
#include <vitex/engine/processors.h>
#include <csignal>

using namespace Vitex::Core;
using namespace Vitex::Compute;
using namespace Vitex::Engine;
using namespace Vitex::Network;

class Runtime : public Application
{
	HTTP::Server* Server = nullptr;
	Stream* Access = nullptr;
	Stream* Error = nullptr;
	Stream* Trace = nullptr;
	Schema* Config = nullptr;
	Console* Log = nullptr;
	String AccessLogs;
	String ErrorLogs;
	String TraceLogs;
	std::mutex Logging;
	bool Requests;
	bool Terminal;

public:
	explicit Runtime(Desc* Conf) : Application(Conf), Requests(true), Terminal(false)
	{
		ErrorHandling::SetFlag(LogOption::Dated, true);
		ErrorHandling::SetCallback(std::bind(&Runtime::OnLog, this, std::placeholders::_1));
		OS::Directory::SetWorking(OS::Directory::GetModule()->c_str());
	}
	~Runtime() override
	{
		VI_CLEAR(Log);
	}
	Promise<void> Shutdown() override
	{
		ErrorHandling::SetCallback(nullptr);
		VI_CLEAR(Server);
		VI_CLEAR(Access);
		VI_CLEAR(Error);
		VI_CLEAR(Trace);
		return Promise<void>::Null();
	}
	void Initialize() override
	{
		auto* Processor = (Processors::ServerProcessor*)Content->GetProcessor<HTTP::Server>();
		Processor->Callback = std::bind(&Runtime::OnConfig, this, std::placeholders::_1, std::placeholders::_2);

		auto NewServer = Content->Load<HTTP::Server>("config.xml");
		if (!NewServer)
		{
			VI_ERR("cannot load server configuration: %s", NewServer.Error().what());
			return Stop();
		}

		Server = *NewServer;
		auto* Router = (HTTP::MapRouter*)Server->GetRouter();
		for (auto It = Router->Listeners.begin(); It != Router->Listeners.end(); It++)
			VI_INFO("listening to \"%s\" %s:%i%s", It->first.c_str(), It->second.Hostname.c_str(), (int)It->second.Port, It->second.Secure ? " (ssl)" : "");

		VI_INFO("searching for sites");
		for (auto& Hoster : Router->Sites)
		{
			auto* Site = Hoster.second;
			VI_INFO("host \"%s\" info", Hoster.first.c_str());
			Site->Base->Callbacks.Headers = &Runtime::OnHeaders;
			if (Requests && !AccessLogs.empty())
				Site->Base->Callbacks.Access = &Runtime::OnAccess;

			VI_INFO("route / is alias for %s", Site->Base->DocumentRoot.c_str());
			for (auto& Group : Site->Groups)
			{
				for (auto Entry : Group->Routes)
				{
					Entry->Callbacks.Headers = &Runtime::OnHeaders;
					if (Requests && !AccessLogs.empty())
						Entry->Callbacks.Access = &Runtime::OnAccess;

					VI_INFO("route %s is alias for %s", Entry->URI.GetRegex().c_str(), Entry->DocumentRoot.c_str());
				}
			}
		}

		if (Config != nullptr)
		{
			Series::Unpack(Config->Fetch("application.threads"), &Control.Threads);
            Series::Unpack(Config->Fetch("application.coroutines"), &Control.Scheduler.MaxCoroutines);
            Series::Unpack(Config->Fetch("application.stack"), &Control.Scheduler.StackSize);
			VI_CLEAR(Config);
		}

		if (!Control.Threads)
		{
			auto Quantity = OS::CPU::GetQuantityInfo();
			Control.Threads = std::max<uint32_t>(2, Quantity.Logical) - 1;
		}

		VI_INFO("queue has %i threads", (int)Control.Threads);
		Server->Listen();

		VI_INFO("setting up signals");
		signal(SIGABRT, OnSignal);
		signal(SIGFPE, OnSignal);
		signal(SIGILL, OnSignal);
		signal(SIGINT, OnSignal);
		signal(SIGSEGV, OnSignal);
		signal(SIGTERM, OnSignal);
#ifdef VI_UNIX
		signal(SIGPIPE, SIG_IGN);
#endif
		VI_INFO("ready to serve and protect");
		ErrorHandling::SetFlag(LogOption::Async, true);
	}
	void OnConfig(void*, Schema* Source)
	{
		Config = Source->Copy();
		Series::Unpack(Config->Fetch("application.log-requests"), &Requests);
		Series::Unpack(Config->Fetch("application.show-terminal"), &Terminal);
		if (Terminal)
		{
			Log = Console::Get();
			Log->Show();
		}
		else
			VI_CLEAR(Log);

		VI_INFO("loading server config from ./config.xml");
		String N = Utils::GetLocalAddress();
		String D = Content->GetEnvironment();

		Series::Unpack(Config->Fetch("application.access-logs"), &AccessLogs);
        OS::Directory::Patch(OS::Path::GetDirectory(AccessLogs.c_str()));

		if (!AccessLogs.empty())
		{
			Stringify::EvalEnvs(AccessLogs, N, D);
			Access = OS::File::OpenArchive(AccessLogs).Or(nullptr);
			VI_INFO("system log (access): %s", AccessLogs.c_str());
		}

		Series::Unpack(Config->Fetch("application.error-logs"), &ErrorLogs);
        OS::Directory::Patch(OS::Path::GetDirectory(ErrorLogs.c_str()));
        
		if (!ErrorLogs.empty())
		{
			Stringify::EvalEnvs(ErrorLogs, N, D);
			Error = OS::File::OpenArchive(ErrorLogs).Or(nullptr);
			VI_INFO("system log (error): %s", ErrorLogs.c_str());
		}

		Series::Unpack(Config->Fetch("application.trace-logs"), &TraceLogs);
        OS::Directory::Patch(OS::Path::GetDirectory(TraceLogs.c_str()));
        
		if (!TraceLogs.empty())
		{
			Stringify::EvalEnvs(TraceLogs, N, D);
			Trace = OS::File::OpenArchive(TraceLogs).Or(nullptr);
			VI_INFO("system log (trace): %s", TraceLogs.c_str());
		}
	}
	void OnLog(ErrorHandling::Details& Data)
	{
		if (Data.Type.Level == LogLevel::Debug || Data.Type.Level == LogLevel::Trace)
		{
			if (Trace != nullptr && Trace->GetResource())
			{
				auto Text = ErrorHandling::GetMessageText(Data);
				UMutex<std::mutex> Unique(Logging);
				Trace->Write(Text.c_str(), Text.size());
			}
		}
		else if (Data.Type.Level == LogLevel::Info)
		{
			if (Access != nullptr && Access->GetResource())
			{
				auto Text = ErrorHandling::GetMessageText(Data);
				UMutex<std::mutex> Unique(Logging);
				Access->Write(Text.c_str(), Text.size());
			}
		}
		else if (Data.Type.Level == LogLevel::Error || Data.Type.Level == LogLevel::Warning)
		{
			if (Error != nullptr && Error->GetResource())
			{
				auto Text = ErrorHandling::GetMessageText(Data);
				UMutex<std::mutex> Unique(Logging);
				Error->Write(Text.c_str(), Text.size());
			}
		}
	}

public:
	static void OnSignal(int Value)
	{
		Application::Get()->Stop();
	}
	static bool OnAccess(HTTP::Connection* Base)
	{
		if (!Base)
			return true;
        
        VI_INFO("%i %s \"%s%s%s\" -> %s / %llub (%llu ms)",
            Base->Response.StatusCode,
            Base->Request.Method,
            Base->Request.Where.c_str(),
            Base->Request.Query.empty() ? "" : "?",
            Base->Request.Query.c_str(),
            Base->RemoteAddress,
            Base->Stream->Outcome,
            Base->Info.Finish - Base->Info.Start);

		return true;
	}
	static bool OnHeaders(HTTP::Connection* Base, String& Content)
	{
		Content.append("Server: lynx\r\n");
		return true;
	}
};

int main()
{
    Application::Desc Init;
    Init.Usage = (size_t)(ApplicationSet::ContentSet | ApplicationSet::NetworkSet);
    Init.Daemon = true;

    Vitex::Runtime Scope((uint64_t)Vitex::Preset::App);
	return Application::StartApp<Runtime>(&Init);
}
