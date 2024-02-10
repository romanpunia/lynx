#include <vitex/vitex.h>
#include <vitex/network/http.h>
#include <vitex/engine/processors.h>

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
		Memory::Release(Log);
	}
	Promise<void> Shutdown() override
	{
		ErrorHandling::SetCallback(nullptr);
		Memory::Release(Server);
		Memory::Release(Access);
		Memory::Release(Error);
		Memory::Release(Trace);
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
		auto Status = Server->Configure(Server->GetRouter());
		if (!Status)
		{
			VI_ERR("cannot configure server: %s", Status.Error().what());
			return Stop();
		}

		auto* Router = (HTTP::MapRouter*)Server->GetRouter();
		for (auto It = Router->Listeners.begin(); It != Router->Listeners.end(); It++)
			VI_INFO("listening to \"%s\" %s:%i%s", It->first.c_str(), It->second.Hostname.c_str(), (int)It->second.Port, It->second.Secure ? " (ssl)" : "");

		Router->Base->Callbacks.Headers = &Runtime::OnHeaders;
		if (Requests && !AccessLogs.empty())
			Router->Base->Callbacks.Access = &Runtime::OnAccess;

		VI_INFO("route / is alias for %s", Router->Base->FilesDirectory.c_str());
		for (auto& Group : Router->Groups)
		{
			for (auto Entry : Group->Routes)
			{
				Entry->Callbacks.Headers = &Runtime::OnHeaders;
				if (Requests && !AccessLogs.empty())
					Entry->Callbacks.Access = &Runtime::OnAccess;

				VI_INFO("route %s is alias for %s", Entry->Location.GetRegex().c_str(), Entry->FilesDirectory.c_str());
			}
		}

		if (Config != nullptr)
		{
			Series::Unpack(Config->Fetch("application.threads"), &Control.Threads);
            Series::Unpack(Config->Fetch("application.coroutines"), &Control.Scheduler.MaxCoroutines);
            Series::Unpack(Config->Fetch("application.stack"), &Control.Scheduler.StackSize);
			Memory::Release(Config);
		}

		if (!Control.Threads)
		{
			auto Quantity = OS::CPU::GetQuantityInfo();
			Control.Threads = std::max<uint32_t>(2, Quantity.Logical) - 1;
		}

		VI_INFO("queue has %i threads", (int)Control.Threads);
		Server->Listen();

		VI_INFO("setting up signals");
		OS::Process::SetSignalCallback(Signal::SIG_ABRT, OnSignal);
		OS::Process::SetSignalCallback(Signal::SIG_FPE, OnSignal);
		OS::Process::SetSignalCallback(Signal::SIG_ILL, OnSignal);
		OS::Process::SetSignalCallback(Signal::SIG_INT, OnSignal);
		OS::Process::SetSignalCallback(Signal::SIG_SEGV, OnSignal);
		OS::Process::SetSignalCallback(Signal::SIG_TERM, OnSignal);
		OS::Process::SetSignalIgnore(Signal::SIG_PIPE);

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
			Memory::Release(Log);

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
			if (Trace != nullptr && Trace->GetWriteable() != nullptr)
			{
				auto Text = ErrorHandling::GetMessageText(Data);
				UMutex<std::mutex> Unique(Logging);
				Trace->Write((uint8_t*)Text.c_str(), Text.size());
			}
		}
		else if (Data.Type.Level == LogLevel::Info)
		{
			if (Access != nullptr && Access->GetWriteable() != nullptr)
			{
				auto Text = ErrorHandling::GetMessageText(Data);
				UMutex<std::mutex> Unique(Logging);
				Access->Write((uint8_t*)Text.c_str(), Text.size());
			}
		}
		else if (Data.Type.Level == LogLevel::Error || Data.Type.Level == LogLevel::Warning)
		{
			if (Error != nullptr && Error->GetWriteable() != nullptr)
			{
				auto Text = ErrorHandling::GetMessageText(Data);
				UMutex<std::mutex> Unique(Logging);
				Error->Write((uint8_t*)Text.c_str(), Text.size());
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
            Base->Request.Referrer.c_str(),
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