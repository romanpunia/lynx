#include <vitex/vitex.h>
#include <vitex/network/http.h>
#include <vitex/layer/processors.h>

using namespace vitex::core;
using namespace vitex::compute;
using namespace vitex::layer;
using namespace vitex::network;

class runtime : public application
{
	http::server* server = nullptr;
	stream* access = nullptr;
	stream* error = nullptr;
	stream* trace = nullptr;
	schema* config = nullptr;
	console* log = nullptr;
	string access_logs;
	string error_logs;
	string trace_logs;
	std::mutex logging;
	bool requests;
	bool terminal;

public:
	explicit runtime(desc* conf) : application(conf), requests(true), terminal(false)
	{
		error_handling::set_flag(log_option::dated, true);
		error_handling::set_callback(std::bind(&runtime::on_log, this, std::placeholders::_1));
		os::directory::set_working(os::directory::get_module()->c_str());
	}
	~runtime() override
	{
		memory::release(log);
	}
	promise<void> shutdown() override
	{
		error_handling::set_callback(nullptr);
		memory::release(server);
		memory::release(access);
		memory::release(error);
		memory::release(trace);
		return promise<void>::null();
	}
	void initialize() override
	{
		auto* processor = (processors::server_processor*)content->get_processor<http::server>();
		processor->callback = std::bind(&runtime::on_config, this, std::placeholders::_1, std::placeholders::_2);

		auto new_server = content->load<http::server>("config.xml");
		if (!new_server)
		{
			VI_ERR("cannot load server configuration: %s", new_server.error().what());
			return stop();
		}

		server = *new_server;
		auto status = server->configure(server->get_router());
		if (!status)
		{
			VI_ERR("cannot configure server: %s", status.error().what());
			return stop();
		}

		auto* router = (http::map_router*)server->get_router();
		for (auto it = router->listeners.begin(); it != router->listeners.end(); it++)
		{
			string hostname = it->second.address.get_hostname().otherwise(string());
			uint16_t port = it->second.address.get_ip_port().otherwise(0);
			VI_INFO("listening to \"%s\" %s:%i%s", it->first.c_str(), hostname.c_str(), (int)port, it->second.is_secure ? " (ssl)" : "");
		}

		router->base->callbacks.headers = &runtime::on_headers;
		if (requests)
			router->base->callbacks.access = &runtime::on_access;

		VI_INFO("route / is alias for %s", router->base->files_directory.c_str());
		for (auto& group : router->groups)
		{
			for (auto entry : group->routes)
			{
				entry->callbacks.headers = &runtime::on_headers;
				if (requests && !access_logs.empty())
					entry->callbacks.access = &runtime::on_access;

				VI_INFO("route %s is alias for %s", entry->location.get_regex().c_str(), entry->files_directory.c_str());
			}
		}

		if (config != nullptr)
		{
			series::unpack(config->fetch("application.threads"), &control.threads);
			series::unpack(config->fetch("application.coroutines"), &control.scheduler.max_coroutines);
			series::unpack(config->fetch("application.stack"), &control.scheduler.stack_size);
			memory::release(config);
		}

		if (!control.threads)
		{
			auto quantity = os::hw::get_quantity_info();
			control.threads = std::max<uint32_t>(2, quantity.logical) - 1;
		}

		VI_INFO("queue has %i threads", (int)control.threads);
		server->listen();

		VI_INFO("setting up signals");
		os::process::bind_signal(signal_code::SIG_ABRT, on_signal);
		os::process::bind_signal(signal_code::SIG_FPE, on_signal);
		os::process::bind_signal(signal_code::SIG_ILL, on_signal);
		os::process::bind_signal(signal_code::SIG_INT, on_signal);
		os::process::bind_signal(signal_code::SIG_SEGV, on_signal);
		os::process::bind_signal(signal_code::SIG_TERM, on_signal);
		os::process::rebind_signal(signal_code::SIG_PIPE);

		VI_INFO("ready to serve and protect");
		error_handling::set_flag(log_option::async, true);
	}
	void on_config(void*, schema* source)
	{
		config = source->copy();
		series::unpack(config->fetch("application.log-requests"), &requests);
		series::unpack(config->fetch("application.show-terminal"), &terminal);
		if (terminal)
		{
			log = console::get();
			log->show();
		}
		else
			memory::release(log);

		VI_INFO("loading server config from ./config.xml");
		vector<string> addresses = utils::get_host_ip_addresses();
		string directory = content->get_environment();

		series::unpack(config->fetch("application.access-logs"), &access_logs);
		os::directory::patch(os::path::get_directory(access_logs.c_str()));

		if (!access_logs.empty())
		{
			stringify::eval_envs(access_logs, directory, addresses);
			access = os::file::open_archive(access_logs).otherwise(nullptr);
			VI_INFO("system log (access): %s", access_logs.c_str());
		}

		series::unpack(config->fetch("application.error-logs"), &error_logs);
		os::directory::patch(os::path::get_directory(error_logs.c_str()));

		if (!error_logs.empty())
		{
			stringify::eval_envs(error_logs, directory, addresses);
			error = os::file::open_archive(error_logs).otherwise(nullptr);
			VI_INFO("system log (error): %s", error_logs.c_str());
		}

		series::unpack(config->fetch("application.trace-logs"), &trace_logs);
		os::directory::patch(os::path::get_directory(trace_logs.c_str()));

		if (!trace_logs.empty())
		{
			stringify::eval_envs(trace_logs, directory, addresses);
			trace = os::file::open_archive(trace_logs).otherwise(nullptr);
			VI_INFO("system log (trace): %s", trace_logs.c_str());
		}
	}
	void on_log(error_handling::details& data)
	{
		if (data.type.level == log_level::debug || data.type.level == log_level::trace)
		{
			if (trace != nullptr && trace->get_writeable() != nullptr)
			{
				auto text = error_handling::get_message_text(data);
				umutex<std::mutex> unique(logging);
				trace->write((uint8_t*)text.c_str(), text.size());
			}
		}
		else if (data.type.level == log_level::info)
		{
			if (access != nullptr && access->get_writeable() != nullptr)
			{
				auto text = error_handling::get_message_text(data);
				umutex<std::mutex> unique(logging);
				access->write((uint8_t*)text.c_str(), text.size());
			}
		}
		else if (data.type.level == log_level::error || data.type.level == log_level::warning)
		{
			if (error != nullptr && error->get_writeable() != nullptr)
			{
				auto text = error_handling::get_message_text(data);
				umutex<std::mutex> unique(logging);
				error->write((uint8_t*)text.c_str(), text.size());
			}
		}
	}

public:
	static void on_signal(int value)
	{
		application::get()->stop();
	}
	static bool on_access(http::connection* base)
	{
		if (!base)
			return true;

		VI_INFO("%i %s \"%s%s%s\" -> %s / %llub (%llu ms)",
			base->response.status_code,
			base->request.method,
			base->request.referrer.c_str(),
			base->request.query.empty() ? "" : "?",
			base->request.query.c_str(),
			base->get_peer_ip_address()->c_str(),
			base->stream->outcome,
			base->info.finish - base->info.start);

		return true;
	}
	static bool on_headers(http::connection* base, string& content)
	{
		content.append("Server: lynx\r\n");
		return true;
	}
};

int main()
{
	application::desc init;
	init.usage = USE_PROCESSING | USE_NETWORKING;
	init.daemon = true;

	vitex::runtime scope;
	return application::start_app<runtime>(&init);
}