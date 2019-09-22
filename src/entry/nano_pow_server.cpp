#include <boost/program_options.hpp>

#include <chrono>
#include <future>

#include <nano_pow/cpp_driver.hpp>
#include <nano_pow/opencl_driver.hpp>
#include <spdlog/logger.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>
#include <workserver/config.hpp>
#include <workserver/util.hpp>
#include <workserver/webserver.hpp>
#include <workserver/work_handler.hpp>

int main (int argc, char * argv[])
{
	boost::program_options::options_description options ("Command line options", 160);
	options.add_options () ("help", "Print out options");
	options.add_options () ("config_path", boost::program_options::value<std::string> ()->default_value ("./nano-pow-server.toml"), "Path to the optional configuration file, including the file name");
	options.add_options () ("config", boost::program_options::value<std::vector<std::string>> ()->multitoken (), "Pass configuration values. This takes precedence over any values in the configuration file. This option can be repeated multiple times.");
	boost::program_options::variables_map vm;
	boost::program_options::store (boost::program_options::command_line_parser (argc, argv).options (options).allow_unregistered ().run (), vm);
	boost::program_options::notify (vm);
	std::vector<std::string> config_overrides;

	if (vm.count ("help") != 0)
	{
		std::cout << options << std::endl;
		std::exit (0);
	}
	if (vm.count ("config"))
	{
		config_overrides = vm["config"].as<std::vector<std::string>> ();
	}

	// Convert from posix path format to native
	std::string config_path (vm["config_path"].as<std::string> ());
	boost::filesystem::path config_path_parsed (config_path);
	std::cout << "Configuration path:" << config_path_parsed.string () << std::endl;
	nano_pow_server::config conf (config_path_parsed.string (), config_overrides);

	// Log configuration
	auto rotating_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt> ("nano-pow-server.log", 1024 * 1024 * 5, 5);
	auto console_sink = std::make_shared<spdlog::sinks::stderr_color_sink_mt> ();
	auto logger = std::make_shared<spdlog::logger> ("logger");
	logger->sinks ().push_back (rotating_sink);
	if (conf.server.log_to_stderr)
	{
		logger->sinks ().push_back (console_sink);
	}
	logger->flush_on (spdlog::level::info);

	// Configure work handler and web server
	nano_pow_server::work_handler work_handler (conf, logger);
	web::config web_conf;
	web_conf.static_pages_allow_remote = conf.admin.allow_remote;
	web::webserver ws (web_conf, conf.server.threads);

	auto work_endpoint_handler = [&](std::string body, std::vector<std::string>, std::shared_ptr<web::http_session> session) {
		work_handler.handle_request_async (body, [session](std::string response) {
			session->write_json_response (response);
		});
	};

	auto work_endpoint_handler_websockets = [&](std::string body, std::shared_ptr<web::websocket_session> session) {
		work_handler.handle_request_async (body, [session](std::string response) {
			session->write_response (response);
		});
	};

	auto work_queue_endpoint_handler = [&](std::string, std::vector<std::string>, std::shared_ptr<web::http_session> session) {
		work_handler.handle_queue_request ([session](std::string response) {
			session->write_json_response (response);
		});
	};

	auto work_queue_delete_endpoint_handler = [&](std::string, std::vector<std::string>, std::shared_ptr<web::http_session> session) {
		work_handler.handle_queue_delete_request ([session](std::string response) {
			session->write_json_response (response);
		});
	};

	auto ping_handler = [&](std::string body, std::vector<std::string> args, std::shared_ptr<web::http_session> session) {
		session->write_json_response (R"({"success": true})");
	};

	auto stop_handler = [&](std::string body, std::vector<std::string> args, std::shared_ptr<web::http_session> session) {
		if (conf.server.allow_control)
		{
			session->write_json_response (R"({"success": true})");
			std::exit (0);
		}
		else
		{
			session->write_json_response (R"({"error": "Control requests are not allowed"})");
		}
	};

	// Clients should use the /api/v1/work endpoint, but POST requests to root is supported for
	// compatibility with the legacy work server
	ws.add_post_endpoint ("/", work_endpoint_handler);
	ws.add_post_endpoint ("/api/v1/work", work_endpoint_handler);
	ws.add_get_endpoint ("/api/v1/work/queue", work_queue_endpoint_handler);
	ws.add_delete_endpoint ("/api/v1/work/queue", work_queue_delete_endpoint_handler);
	ws.add_get_endpoint ("/api/v1/ping", ping_handler);
	ws.add_get_endpoint ("/api/v1/stop", stop_handler);
	ws.add_websocket_endpoint ("/api/v1/work", work_endpoint_handler_websockets);

	logger->info ("Nano PoW Server started");
	ws.start (conf.server.bind_address, conf.server.port, conf.admin.doc_root);

	return EXIT_SUCCESS;
}
