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
	bool Terminal;

public:
	explicit Runtime(Desc* Conf) : Application(Conf), Terminal(false)
	{
		OS::SetLogCallback([this](const char* Value, int Level)
		{
			this->OnLogCallback(Value, Level);
		});
	}
	~Runtime() override
	{
	}
	void Initialize() override
	{
		auto* Processor = (Processors::Server*)Content->GetProcessor<HTTP::Server>();
		Processor->Callback = [this](void*, Schema* Doc) -> void
		{
			this->OnLoadLibrary(Doc);
		};

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
			if (!AccessLogs.empty())
				Site->Base->Callbacks.Access = Runtime::OnLogAccess;

			TH_INFO("route / is alias for %s", Site->Base->DocumentRoot.c_str());
			for (auto& Group : Site->Groups)
			{
				for (auto Entry : Group.Routes)
				{
					Entry->Callbacks.Headers = Runtime::OnHeaders;
					if (!AccessLogs.empty())
						Entry->Callbacks.Access = Runtime::OnLogAccess;

					TH_INFO("route %s is alias for %s", Entry->URI.GetRegex().c_str(), Entry->DocumentRoot.c_str());
				}
			}
		}

		if (Reference != nullptr)
		{
			NMake::Unpack(Reference->Fetch("application.threads"), &Control.Threads);
            NMake::Unpack(Reference->Fetch("application.coroutines"), &Control.Coroutines);
            NMake::Unpack(Reference->Fetch("application.stack"), &Control.Stack);
			TH_CLEAR(Reference);
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
		NMake::Unpack(Schema->Fetch("application.terminal"), &Terminal);
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

		NMake::Unpack(Schema->Fetch("application.access-logs"), &AccessLogs);
		if (!AccessLogs.empty())
		{
			Access = new FileStream();
			if (!Access->Open(Parser(&AccessLogs).Eval(N, D).Get(), FileMode::Binary_Append_Only))
				TH_CLEAR(Access);

			TH_INFO("system log (access): %s", AccessLogs.c_str());
		}

		NMake::Unpack(Schema->Fetch("application.error-logs"), &ErrorLogs);
		if (!ErrorLogs.empty())
		{
			Error = new FileStream();
			if (!Error->Open(Parser(&ErrorLogs).Eval(N, D).Get(), FileMode::Binary_Append_Only))
				TH_CLEAR(Error);

			TH_INFO("system log (error): %s", ErrorLogs.c_str());
		}

		NMake::Unpack(Schema->Fetch("application.trace-logs"), &TraceLogs);
		if (!TraceLogs.empty())
		{
			Trace = new FileStream();
			if (!Trace->Open(Parser(&TraceLogs).Eval(N, D).Get(), FileMode::Binary_Append_Only))
				TH_CLEAR(Trace);

			TH_INFO("system log (trace): %s", TraceLogs.c_str());
		}

		NMake::Unpack(Schema->Fetch("application.file-directory"), &RootDirectory);
		Parser(&RootDirectory).Eval(N, D);
		Reference = Schema->Copy();

		TH_INFO("tmp file directory root is %s", RootDirectory.c_str());
	}
	void OnLogCallback(const char* Value, int Level)
	{
		if (Level == 4)
		{
			if (Trace != nullptr && Trace->GetBuffer())
				Trace->Write(Value, strlen(Value));
		}
		else if (Level == 3)
		{
			if (Access != nullptr && Access->GetBuffer())
				Access->Write(Value, strlen(Value));
		}
		else if (Level == 1 || Level == 2)
		{
			if (Error != nullptr && Error->GetBuffer())
				Error->Write(Value, strlen(Value));
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

		TH_INFO("%s %s \"%s%s%s\" %i - %s / %llub [%llums]", Base->Request.Method, Base->Request.Version, Base->Request.URI.c_str(), Base->Request.Query.empty() ? "" : "?", Base->Request.Query.c_str(), Base->Response.StatusCode, Base->Request.RemoteAddress, Base->Stream->Outcome, Base->Info.Finish - Base->Info.Start);
		return true;
	}
	static bool OnHeaders(HTTP::Connection* Base, Parser* Content)
	{
		if (Content != nullptr)
			Content->Append("Server: Lynx\r\n");

		return true;
	}
};

int main()
{
	Tomahawk::Initialize((uint64_t)Tomahawk::Preset::App);
	{
		Application::Desc Interface;
		Interface.Usage = (size_t)(ApplicationSet::ContentSet | ApplicationSet::NetworkSet);
		Interface.Directory.clear();
		Interface.Daemon = true;
		Interface.Async = true;

		auto* App = new Runtime(&Interface);
		App->Start();
		TH_RELEASE(App);
	}
	Tomahawk::Uninitialize();

	return 0;
}
