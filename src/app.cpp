#include <edge/edge.h>
#include <edge/core/network.h>
#include <edge/core/engine.h>
#include <edge/network/http.h>
#include <edge/engine/processors.h>
#include <csignal>

using namespace Edge::Core;
using namespace Edge::Compute;
using namespace Edge::Engine;
using namespace Edge::Network;

class Runtime : public Application
{
	HTTP::Server* Server = nullptr;
	Stream* Access = nullptr;
	Stream* Error = nullptr;
	Stream* Trace = nullptr;
	Schema* Reference = nullptr;
	Console* Log = nullptr;
	String AccessLogs;
	String ErrorLogs;
	String TraceLogs;
	String RootDirectory;
	std::mutex Logging;
	bool Requests;
	bool Terminal;

public:
	explicit Runtime(Desc* Conf) : Application(Conf), Requests(true), Terminal(false)
	{
		OS::SetLogCallback(std::bind(&Runtime::OnLogCallback, this, std::placeholders::_1));
	}
	~Runtime() override
	{
		ED_RELEASE(Log);
	}
	void Initialize() override
	{
		auto* Processor = (Processors::ServerProcessor*)Content->GetProcessor<HTTP::Server>();
		Processor->Callback = [this](void*, Schema* Doc) { OnLoadLibrary(Doc); };

		Server = Content->Load<HTTP::Server>("config.xml");
		if (!Server)
		{
			ED_ERR("an error occurred while loading config");
			return Stop();
		}

		auto* Router = (HTTP::MapRouter*)Server->GetRouter();
		for (auto It = Router->Listeners.begin(); It != Router->Listeners.end(); It++)
			ED_INFO("listening to \"%s\" %s:%i%s", It->first.c_str(), It->second.Hostname.c_str(), (int)It->second.Port, It->second.Secure ? " (ssl)" : "");

		ED_INFO("searching for sites");
		for (auto& Hoster : Router->Sites)
		{
			auto* Site = Hoster.second;
			ED_INFO("host \"%s\" info", Hoster.first.c_str());
			Site->Base->Callbacks.Headers = Runtime::OnHeaders;
			if (Requests && !AccessLogs.empty())
				Site->Base->Callbacks.Access = Runtime::OnLogAccess;

			ED_INFO("route / is alias for %s", Site->Base->DocumentRoot.c_str());
			for (auto& Group : Site->Groups)
			{
				for (auto Entry : Group->Routes)
				{
					Entry->Callbacks.Headers = Runtime::OnHeaders;
					if (Requests && !AccessLogs.empty())
						Entry->Callbacks.Access = Runtime::OnLogAccess;

					ED_INFO("route %s is alias for %s", Entry->URI.GetRegex().c_str(), Entry->DocumentRoot.c_str());
				}
			}
		}

		if (Reference != nullptr)
		{
			Series::Unpack(Reference->Fetch("application.threads"), &Control.Threads);
            Series::Unpack(Reference->Fetch("application.coroutines"), &Control.Coroutines);
            Series::Unpack(Reference->Fetch("application.stack"), &Control.Stack);
			ED_CLEAR(Reference);
		}

		if (!Control.Threads)
		{
			auto Quantity = OS::CPU::GetQuantityInfo();
			Control.Threads = std::max<uint32_t>(2, Quantity.Logical) - 1;
		}

		ED_INFO("queue has %i threads", (int)Control.Threads);
		Server->Listen();

		ED_INFO("setting up signals");
		signal(SIGABRT, OnAbort);
		signal(SIGFPE, OnArithmeticError);
		signal(SIGILL, OnIllegalOperation);
		signal(SIGINT, OnCtrl);
		signal(SIGSEGV, OnInvalidAccess);
		signal(SIGTERM, OnTerminate);
#ifdef ED_UNIX
		signal(SIGPIPE, SIG_IGN);
#endif
		ED_INFO("ready to serve and protect");
		OS::SetLogDeferred(true);
	}
	void CloseEvent() override
	{
		OS::SetLogCallback(nullptr);
		ED_RELEASE(Server);
		ED_RELEASE(Access);
		ED_RELEASE(Error);
		ED_RELEASE(Trace);
	}
	void OnLoadLibrary(Schema* Schema)
	{
		Series::Unpack(Schema->Fetch("application.log-requests"), &Requests);
		Series::Unpack(Schema->Fetch("application.show-terminal"), &Terminal);
		if (Terminal)
		{
			Log = Console::Get();
			Log->Show();
		}
		else
			ED_CLEAR(Log);

		ED_INFO("loading server config from ./config.xml");
		String N = Multiplexer::GetLocalAddress();
		String D = Content->GetEnvironment();

		Series::Unpack(Schema->Fetch("application.access-logs"), &AccessLogs);
        OS::Directory::Patch(OS::Path::GetDirectory(AccessLogs.c_str()));

		if (!AccessLogs.empty())
		{
			Access = OS::File::OpenArchive(Stringify(&AccessLogs).Eval(N, D).R());
			ED_INFO("system log (access): %s", AccessLogs.c_str());
		}

		Series::Unpack(Schema->Fetch("application.error-logs"), &ErrorLogs);
        OS::Directory::Patch(OS::Path::GetDirectory(ErrorLogs.c_str()));
        
		if (!ErrorLogs.empty())
		{
			Error = OS::File::OpenArchive(Stringify(&ErrorLogs).Eval(N, D).R());
			ED_INFO("system log (error): %s", ErrorLogs.c_str());
		}

		Series::Unpack(Schema->Fetch("application.trace-logs"), &TraceLogs);
        OS::Directory::Patch(OS::Path::GetDirectory(TraceLogs.c_str()));
        
		if (!TraceLogs.empty())
		{
			Trace = OS::File::OpenArchive(Stringify(&TraceLogs).Eval(N, D).R());
			ED_INFO("system log (trace): %s", TraceLogs.c_str());
		}

		Series::Unpack(Schema->Fetch("application.file-directory"), &RootDirectory);
		Stringify(&RootDirectory).Eval(N, D);
		Reference = Schema->Copy();
		
		ED_INFO("tmp file directory root is %s", RootDirectory.c_str());
	}
	void OnLogCallback(OS::Message& Data)
	{
		auto& Text = Data.GetText();
		if (Data.Level == 4 || Data.Level == 5)
		{
			if (Trace != nullptr && Trace->GetBuffer())
			{
				std::unique_lock<std::mutex> Unique(Logging);
				Trace->Write(Text.c_str(), Text.size());
			}
		}
		else if (Data.Level == 3)
		{
			if (Access != nullptr && Access->GetBuffer())
			{
				std::unique_lock<std::mutex> Unique(Logging);
				Access->Write(Text.c_str(), Text.size());
			}
		}
		else if (Data.Level == 1 || Data.Level == 2)
		{
			if (Error != nullptr && Error->GetBuffer())
			{
				std::unique_lock<std::mutex> Unique(Logging);
				Error->Write(Text.c_str(), Text.size());
			}
		}
	}

public:
	static bool CanTerminate()
	{
		static std::mutex Mutex;
		static bool Termination = false;

		Mutex.lock();
		bool Value = !Termination;
		Termination = true;
		Mutex.unlock();

		return Value;
	}
	static void OnAbort(int Value)
	{
		signal(SIGABRT, OnAbort);
		if (CanTerminate())
			Application::Get()->Stop();
	}
	static void OnArithmeticError(int Value)
	{
		signal(SIGFPE, OnArithmeticError);
		if (CanTerminate())
			Application::Get()->Stop();
	}
	static void OnIllegalOperation(int Value)
	{
		signal(SIGILL, OnIllegalOperation);
		if (CanTerminate())
			Application::Get()->Stop();
	}
	static void OnCtrl(int Value)
	{
		signal(SIGINT, OnCtrl);
		if (CanTerminate())
			Application::Get()->Stop();
	}
	static void OnInvalidAccess(int Value)
	{
		signal(SIGSEGV, OnInvalidAccess);
		if (CanTerminate())
			Application::Get()->Stop();
	}
	static void OnTerminate(int Value)
	{
		signal(SIGTERM, OnTerminate);
		if (CanTerminate())
			Application::Get()->Stop();
	}
	static bool OnLogAccess(HTTP::Connection* Base)
	{
		if (!Base)
			return true;
        
        ED_INFO("%i %s \"%s%s%s\" -> %s / %llub (%llu ms)",
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
	static bool OnHeaders(HTTP::Connection* Base, Stringify* Content)
	{
		if (Content != nullptr)
			Content->Append("Server: lynx\r\n");

		return true;
	}
};

int main()
{
    Application::Desc Init;
    Init.Usage = (size_t)(ApplicationSet::ContentSet | ApplicationSet::NetworkSet);
    Init.Daemon = true;

    Edge::Initialize((uint64_t)Edge::Preset::App);
    int ExitCode = Application::StartApp<Runtime>(&Init);
    Edge::Uninitialize();

	return ExitCode;
}
