#include <tomahawk/tomahawk.h>
#include <csignal>

using namespace Tomahawk::Core;
using namespace Tomahawk::Audio;
using namespace Tomahawk::Compute;
using namespace Tomahawk::Engine;
using namespace Tomahawk::Network;
using namespace Tomahawk::Graphics;
using namespace Tomahawk::Script;

class Runtime : public Application
{
	HTTP::Server* Server = nullptr;
	FileStream* Access = nullptr;
	FileStream* Error = nullptr;
	FileStream* Trace = nullptr;
	Schema* Reference = nullptr;
	Console* Log = nullptr;
	std::string AccessLogs;
	std::string ErrorLogs;
	std::string TraceLogs;
	std::string RootDirectory;
	bool Requests;
	bool Terminal;

public:
	explicit Runtime(Desc* Conf) : Application(Conf), Terminal(false)
	{
		OS::SetLogCallback([this](OS::Message& Data)
		{
			this->OnLogCallback(Data);
		});
	}
	~Runtime() override
	{
	}
	void Initialize() override
	{
		auto* Processor = (Processors::Server*)Content->GetProcessor<HTTP::Server>();
		Processor->Callback = [this](void*, Schema* Doc) { OnLoadLibrary(Doc); };

		Server = Content->Load<HTTP::Server>("config.xml");
		if (!Server)
		{
			TH_ERR("an error occurred while loading config");
			return Stop();
		}

		auto* Router = (HTTP::MapRouter*)Server->GetRouter();
		for (auto It = Router->Listeners.begin(); It != Router->Listeners.end(); It++)
			TH_INFO("listening to \"%s\" %s:%i%s", It->first.c_str(), It->second.Hostname.c_str(), (int)It->second.Port, It->second.Secure ? " (ssl)" : "");

		TH_INFO("searching for sites");
		for (auto& Hoster : Router->Sites)
		{
			auto* Site = Hoster.second;
			TH_INFO("host \"%s\" info", Hoster.first.c_str());
			Site->Base->Callbacks.Headers = Runtime::OnHeaders;
			if (Requests && !AccessLogs.empty())
				Site->Base->Callbacks.Access = Runtime::OnLogAccess;

			TH_INFO("route / is alias for %s", Site->Base->DocumentRoot.c_str());
			for (auto& Group : Site->Groups)
			{
				for (auto Entry : Group.Routes)
				{
					Entry->Callbacks.Headers = Runtime::OnHeaders;
					if (Requests && !AccessLogs.empty())
						Entry->Callbacks.Access = Runtime::OnLogAccess;

					TH_INFO("route %s is alias for %s", Entry->URI.GetRegex().c_str(), Entry->DocumentRoot.c_str());
				}
			}
		}

		if (Reference != nullptr)
		{
			Series::Unpack(Reference->Fetch("application.threads"), &Control.Threads);
            Series::Unpack(Reference->Fetch("application.coroutines"), &Control.Coroutines);
            Series::Unpack(Reference->Fetch("application.stack"), &Control.Stack);
			TH_CLEAR(Reference);
		}

		if (!Control.Threads)
		{
			auto Quantity = OS::CPU::GetQuantityInfo();
			Control.Threads = std::max<uint32_t>(2, Quantity.Logical) - 1;
		}

		TH_INFO("queue has %i threads", (int)Control.Threads);
		Server->Listen();

		TH_INFO("setting up signals");
		signal(SIGABRT, OnAbort);
		signal(SIGFPE, OnArithmeticError);
		signal(SIGILL, OnIllegalOperation);
		signal(SIGINT, OnCtrl);
		signal(SIGSEGV, OnInvalidAccess);
		signal(SIGTERM, OnTerminate);
#ifdef TH_UNIX
		signal(SIGPIPE, SIG_IGN);
#endif
		TH_INFO("ready to serve and protect");
		OS::SetLogDeferred(true);
	}
	void CloseEvent() override
	{
		OS::SetLogCallback(nullptr);
		delete Server;
		delete Log;
		delete Access;
		delete Error;
		delete Trace;
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
			TH_CLEAR(Log);

		TH_INFO("loading server config from ./config.xml");
		std::string N = Socket::GetLocalAddress();
		std::string D = Content->GetEnvironment();

		Series::Unpack(Schema->Fetch("application.access-logs"), &AccessLogs);
        OS::Directory::Patch(OS::Path::GetDirectory(AccessLogs.c_str()));

		if (!AccessLogs.empty())
		{
			Access = new FileStream();
			if (!Access->Open(Parser(&AccessLogs).Eval(N, D).Get(), FileMode::Binary_Append_Only))
				TH_CLEAR(Access);

			TH_INFO("system log (access): %s", AccessLogs.c_str());
		}

		Series::Unpack(Schema->Fetch("application.error-logs"), &ErrorLogs);
        OS::Directory::Patch(OS::Path::GetDirectory(ErrorLogs.c_str()));
        
		if (!ErrorLogs.empty())
		{
			Error = new FileStream();
			if (!Error->Open(Parser(&ErrorLogs).Eval(N, D).Get(), FileMode::Binary_Append_Only))
				TH_CLEAR(Error);

			TH_INFO("system log (error): %s", ErrorLogs.c_str());
		}

		Series::Unpack(Schema->Fetch("application.trace-logs"), &TraceLogs);
        OS::Directory::Patch(OS::Path::GetDirectory(TraceLogs.c_str()));
        
		if (!TraceLogs.empty())
		{
			Trace = new FileStream();
			if (!Trace->Open(Parser(&TraceLogs).Eval(N, D).Get(), FileMode::Binary_Append_Only))
				TH_CLEAR(Trace);

			TH_INFO("system log (trace): %s", TraceLogs.c_str());
		}

		Series::Unpack(Schema->Fetch("application.file-directory"), &RootDirectory);
		Parser(&RootDirectory).Eval(N, D);
		Reference = Schema->Copy();

		TH_INFO("tmp file directory root is %s", RootDirectory.c_str());
	}
	void OnLogCallback(OS::Message& Data)
	{
		auto& Text = Data.GetText();
		if (Data.Level == 4)
		{
			if (Trace != nullptr && Trace->GetBuffer())
				Trace->Write(Text.c_str(), Text.size());
		}
		else if (Data.Level == 3)
		{
			if (Access != nullptr && Access->GetBuffer())
				Access->Write(Text.c_str(), Text.size());
		}
		else if (Data.Level == 1 || Data.Level == 2)
		{
			if (Error != nullptr && Error->GetBuffer())
				Error->Write(Text.c_str(), Text.size());
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
        
        TH_INFO("%i %s \"%s%s%s\" -> %s / %llub (%llu ms)",
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
	static bool OnHeaders(HTTP::Connection* Base, Parser* Content)
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

    Tomahawk::Initialize((uint64_t)Tomahawk::Preset::App);
    int ExitCode = Application::StartApp<Runtime>(&Init);
    Tomahawk::Uninitialize();

	return ExitCode;
}
