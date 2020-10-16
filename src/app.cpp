#include <tomahawk/tomahawk.h>
#include <csignal>

using namespace Tomahawk::Rest;
using namespace Tomahawk::Audio;
using namespace Tomahawk::Compute;
using namespace Tomahawk::Engine;
using namespace Tomahawk::Network;
using namespace Tomahawk::Graphics;
using namespace Tomahawk::Script;

class Runtime : public Application
{
    HTTP::Server* Server = nullptr;
    Document* Reference = nullptr;
    Console* Log = nullptr;
    FileStream* Access = nullptr;
    FileStream* Error = nullptr;
    FileStream* Trace = nullptr;
    std::string AccessLogs;
    std::string ErrorLogs;
    std::string TraceLogs;
    std::string RootDirectory;
    bool Terminal;
    bool ForceQuit;

public:
    explicit Runtime(Desc* Conf) : Application(Conf), Terminal(false), ForceQuit(false)
    {
        Debug::AttachCallback([this](const char* Value, int Level)
        {
            this->OnLogCallback(Value, Level);
        });
    }
    ~Runtime() override
    {
        TH_RELEASE(Server);
		TH_RELEASE(Log);

        Debug::DetachCallback();
		TH_RELEASE(Access);
		TH_RELEASE(Error);
		TH_RELEASE(Trace);

        if (ForceQuit)
            exit(0);
    }
    void Initialize(Desc* Conf) override
    {
        Content->SetEnvironment(OS::GetDirectory() + "lynx");
        Content->GetProcessor<HTTP::Server>()->As<Processors::Server>()->Callback = [this](void*, Document* Doc) -> void
        {
            this->OnLoadLibrary(Doc);
        };

        Server = Content->Load<HTTP::Server>("conf.xml");
        if (!Server)
        {
            TH_ERROR("an error occurred while loading config");
            return Restate(ApplicationState_Terminated);
        }

        auto Router = (HTTP::MapRouter*)Server->GetRouter();
        for (auto It = Router->Listeners.begin(); It != Router->Listeners.end(); It++)
            TH_INFO("listening to \"%s\" %s:%i%s", It->first.c_str(), It->second.Hostname.c_str(), (int)It->second.Port, It->second.Secure ? " (ssl)" : "");

        TH_INFO("searching for sites");
        for (auto& Site : Router->Sites)
        {
            TH_INFO("site \"%s\" info", Site->SiteName.c_str());
            for (auto It = Site->Hosts.begin(); It != Site->Hosts.end(); It++)
                TH_INFO("site \"%s\" is attached to %s", Site->SiteName.c_str(), It->c_str());

            TH_INFO("configuring routes");
            Site->Base->Callbacks.Headers = Runtime::OnHeaders;
            if (!AccessLogs.empty())
                Site->Base->Callbacks.Access = Runtime::OnLogAccess;

            TH_INFO("route / is alias for %s", Site->Base->DocumentRoot.c_str());
            for (auto Entry : Site->Routes)
            {
                Entry->Callbacks.Headers = Runtime::OnHeaders;
                if (!AccessLogs.empty())
                    Entry->Callbacks.Access = Runtime::OnLogAccess;

                TH_INFO("route %s is alias for %s", Entry->URI.Regex.c_str(), Entry->DocumentRoot.c_str());
            }
        }

        if (Reference != nullptr)
        {
            NMake::Unpack(Reference->FindPath("application.task-workers-count"), &Conf->TaskWorkersCount);
            NMake::Unpack(Reference->FindPath("application.event-workers-count"), &Conf->EventWorkersCount);
            TH_CLEAR(Reference);
        }

        TH_INFO("internal queue has %i task workers and %i event workers", (int)Conf->TaskWorkersCount, (int)Conf->EventWorkersCount);
        Server->Listen(Application::Get()->Queue);
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
    void OnLoadLibrary(Document* Document)
    {
        NMake::Unpack(Document->FindPath("application.terminal"), &Terminal);
        if (Terminal)
        {
            Debug::AttachStream();
            Log = Console::Get();
            Log->Show();
        }
        else
        {
            Debug::DetachStream();
			TH_CLEAR(Log);
        }

        TH_INFO("loading server config from ./lynx/conf.xml");
        std::string N = Socket::LocalIpAddress();
        std::string D = Content->GetEnvironment();

        NMake::Unpack(Document->FindPath("application.access-logs"), &AccessLogs);
        if (!AccessLogs.empty())
        {
            TH_RELEASE(Access);

            Access = new FileStream();
            if (!Access->Open(Stroke(&AccessLogs).Path(N, D).Get(), FileMode_Binary_Append_Only))
				TH_CLEAR(Access);

            TH_INFO("system log (access): %s", AccessLogs.c_str());
        }

        NMake::Unpack(Document->FindPath("application.error-logs"), &ErrorLogs);
        if (!ErrorLogs.empty())
        {
			TH_RELEASE(Error);

            Error = new FileStream();
            if (!Error->Open(Stroke(&ErrorLogs).Path(N, D).Get(), FileMode_Binary_Append_Only))
				TH_CLEAR(Error);

            TH_INFO("system log (error): %s", ErrorLogs.c_str());
        }

        NMake::Unpack(Document->FindPath("application.trace-logs"), &TraceLogs);
        if (!TraceLogs.empty())
        {
			TH_RELEASE(Trace);

            Trace = new FileStream();
            if (!Trace->Open(Stroke(&TraceLogs).Path(N, D).Get(), FileMode_Binary_Append_Only))
				TH_CLEAR(Trace);

            TH_INFO("system log (trace): %s", TraceLogs.c_str());
        }

        NMake::Unpack(Document->FindPath("application.file-directory"), &RootDirectory);
        NMake::Unpack(Document->FindPath("application.force-quit"), &ForceQuit);
        Stroke(&RootDirectory).Path(N, D);
        Reference = Document->Copy();

        TH_INFO("tmp file directory root is %s", RootDirectory.c_str());
        if (ForceQuit)
            TH_INFO("server will be forced to shutdown");
    }
    void OnLogCallback(const char* Value, int Level)
    {
        if (Level == 3 || Level == 4)
        {
            if (Trace != nullptr)
                Trace->Write(Value, strlen(Value));
        }
        else if (Level == 0)
        {
            if (Access != nullptr)
                Access->Write(Value, strlen(Value));
        }
        else if (Level == 1 || Level == 2)
        {
            if (Error != nullptr)
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
            Application::Get()->As<Runtime>()->Restate(ApplicationState_Terminated);
    }
    static void OnArithmeticError(int Value)
    {
        signal(SIGFPE, OnArithmeticError);
        if (CanTerminate())
            Application::Get()->As<Runtime>()->Restate(ApplicationState_Terminated);
    }
    static void OnIllegalOperation(int Value)
    {
        signal(SIGILL, OnIllegalOperation);
        if (CanTerminate())
            Application::Get()->As<Runtime>()->Restate(ApplicationState_Terminated);
    }
    static void OnCtrl(int Value)
    {
        signal(SIGINT, OnCtrl);
        if (CanTerminate())
            Application::Get()->As<Runtime>()->Restate(ApplicationState_Terminated);
    }
    static void OnInvalidAccess(int Value)
    {
        signal(SIGSEGV, OnInvalidAccess);
        if (CanTerminate())
            Application::Get()->As<Runtime>()->Restate(ApplicationState_Terminated);
    }
    static void OnTerminate(int Value)
    {
        signal(SIGTERM, OnTerminate);
        if (CanTerminate())
            Application::Get()->As<Runtime>()->Restate(ApplicationState_Terminated);
    }
    static bool OnLogAccess(HTTP::Connection* Base)
    {
        if (!Base)
            return true;

        TH_INFO("%s %s \"%s%s%s\" %i - %s / %llub [%llums]", Base->Request.Method, Base->Request.Version, Base->Request.URI.c_str(), Base->Request.Query.empty() ? "" : "?", Base->Request.Query.c_str(), Base->Response.StatusCode, Base->Request.RemoteAddress, Base->Stream->Outcome, Base->Info.Finish - Base->Info.Start);

        return true;
    }
    static bool OnHeaders(HTTP::Connection* Base, Stroke* Content)
    {
        if (Content != nullptr)
            Content->Append("Server: Lynx\r\n");

        return true;
    }
};

int main()
{
    Tomahawk::Initialize(Tomahawk::TInit_All, Tomahawk::TMem_2MB);
    {
        Application::Desc Interface;
        Interface.Threading = EventWorkflow_Mixed;
        Interface.Usage = ApplicationUse_Content_Module;
        Interface.FrameLimit = 6;

        auto App = new Runtime(&Interface);
        App->Run(&Interface);
        TH_RELEASE(App);
    }
    Tomahawk::Uninitialize();

    return 0;
}