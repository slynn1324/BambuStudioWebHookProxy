/**
 * BSWHP -- Bambu Studio Web Hook Proxy
 * 
 * A dynamic library that proxies the bambu_networking library that is part of the closed source Bambu Studio network plugin.  
 * 
 * All calls are passed through unmodified with the exception of `bambu_network_set_on_local_message_fn`, which uses an interceptor
 * function to invoke a configured webhook url with the json message status from the printer.
 * 
 * This is necessary to work around an issue with the Bambu P1 LAN mode where only a single concurrent MQTT connection is functional.
 * 
 * 
 * Note: it appears that Bambu Labs does not maintain backwards compatibility with the method signatures expected between versions
 * of Bambu Studio and the network plugins - so this plugin must be matched to the version of the library in use or it will likely break/segfault.
 * 
 * Dependencies from the BambuStudio src:
 * 	BambuStudio/src/slic3r/Utils/bambu_networking.hpp
 *  BambuStudio/src/libslic3r/ProjectTask.hpp
 * 
 * Function signatures derived from BambuStudio/src/slic3r/Utils/NetworkAgent.hpp
 * 
 */ 
#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <fstream>
#include <string>
#include <iostream>

#include "bambu_networking.hpp"
#include "ProjectTask.hpp"

#include "HTTPRequest.hpp"

using namespace BBL;

namespace Slic3r {

std::string _BSWHP_EXPECTED_LIB_VERSION = "01.08.00.03";

const char* _BSWHP_ws = " \t\n\r\f\v";
std::string& _BSWHP_rtrim(std::string& s, const char* t = _BSWHP_ws){
	s.erase(s.find_last_not_of(t) + 1);
	return s;
}

std::string& _BSWHP_ltrim(std::string& s, const char* t = _BSWHP_ws){
	s.erase(0, s.find_first_not_of(t));
	return s;
}

std::string& _BSWHP_trim(std::string& s, const char* t = _BSWHP_ws){
	return _BSWHP_ltrim(_BSWHP_rtrim(s, t), t);
}

std::string _BSWHP_get_env(const char * name){
	const char * val = getenv(name);
	return val == NULL ? std::string() : std::string(val);
}



bool _BSWHP_debug_init = false;
std::string _BSWHP_debug = "0";
bool _BSWHP_is_debug(){
	if ( ! _BSWHP_debug_init ){
		std::string home_dir = _BSWHP_get_env("HOME");
		std::string debugFilePath = std::string(home_dir) + "/Library/Application Support/BambuStudioBeta/bswhp_debug.txt";
		std::ifstream debugFile(debugFilePath.c_str());
		if ( debugFile.is_open() ){
			debugFile >> _BSWHP_debug;
			_BSWHP_trim(_BSWHP_debug);
		}

		_BSWHP_debug_init = true;

		std::cout << "BSWHP DEBUG: " << _BSWHP_debug.c_str();
	}
	return "1" == _BSWHP_debug || "2" == _BSWHP_debug;
}

bool _BSWHP_is_verbose(){
	bool debug = _BSWHP_is_debug();
	return "2" == _BSWHP_debug;
}


void _BSWHPLOG(const char * format, ...){
	if ( _BSWHP_is_debug() ){
		printf("BSWHP_LOG: ");
		va_list args;
		va_start(args, format);
		vprintf(format, args);
		va_end(args);
		printf("\n");
	}
}

void _BSWHPLOG_VERBOSE(const char * format, ...){
	if ( _BSWHP_is_verbose() ){
		printf("BSWHP_LOG_VERBOSE: ");
		va_list args;
		va_start(args, format);
		vprintf(format, args);
		va_end(args);
		printf("\n");
	}
}


bool _BSWHP_URL_INIT = false;
std::string _BSWHP_URL = "";
std::string _BSWHP_get_url(){
	if ( !_BSWHP_URL_INIT ){

		std::string home_dir = _BSWHP_get_env("HOME");
		std::string urlFilePath = std::string(home_dir) + "/Library/Application Support/BambuStudioBeta/bswhp_url.txt";

		std::ifstream urlFile(urlFilePath.c_str());
		if ( urlFile.is_open() ){
			urlFile >> _BSWHP_URL;
			_BSWHP_trim(_BSWHP_URL);
		}

		_BSWHP_URL_INIT = true;

		_BSWHPLOG("BSWHP URL: %s", _BSWHP_URL.c_str());
	}
	return _BSWHP_URL;
}


// library and function loading methods

static void * _BSWHP_LIBRARY = NULL;
void * _BSWHP_get_library(){
	if (!_BSWHP_LIBRARY){
		std::string home_dir = _BSWHP_get_env("HOME");
		if ( home_dir.length() > 0 ){
			std::string path = std::string(home_dir) + "/Library/Application Support/BambuStudioBeta/plugins/o_libbambu_networking.dylib";
			_BSWHP_LIBRARY = dlopen( path.c_str(), RTLD_LAZY );
		} else {
			std::cerr << "ERROR: NO $HOME ENVIRONMENT VARIABLE, UNABLE TO DETERMINE LIBRARY LOCATION\n";
			exit(1);
		}
	}

	std::string libVersion = ((std::string(*)()) dlsym(_BSWHP_LIBRARY,"bambu_network_get_version") )();
	
	if ( libVersion != _BSWHP_EXPECTED_LIB_VERSION ){
		std::cerr << "WARNING: unexpected libbambu_networking version!\n    version: " << libVersion << "\n    " << _BSWHP_EXPECTED_LIB_VERSION << "\n";
	} else {
		std::cout << "BSWHP: matched library version: " << libVersion << "\n";
	}

	return _BSWHP_LIBRARY;
}

void * _BSWHP_get_function(const char * name){
	void * library = _BSWHP_get_library();
	_BSWHPLOG_VERBOSE("getting function %s", name);
	void * function = dlsym(library, name);
	_BSWHPLOG_VERBOSE("got function %s", name);
	return function;
}


// wrap the on_local_message function to invoke webhook if defined

OnMessageFn _BSWHP_on_message_fn = NULL;

void _BSWHP_on_local_message_fn(std::string dev_id, std::string msg){
	_BSWHPLOG("_BSWHP_on_local_message_fn\n  dev_id: %s\n  msg: %s", dev_id.c_str(), msg.c_str());

	if ( _BSWHP_get_url().length() > 0 ){
		_BSWHPLOG("webhook_url: %s", _BSWHP_URL.c_str());
		try {

			http::Request request{_BSWHP_URL};
			const std::string body = msg;
			const auto response = request.send("POST", body, {
				{"Content-Type", "application/json"},
				{"dev_id", dev_id}
			});

		} catch (const std::exception& e){
			std::cerr << "Request Failed, error: " << e.what() << "\n";
		}
	} else {
		_BSWHPLOG("no webhook_url");
	}

	_BSWHP_on_message_fn(dev_id, msg);
}


// tweaked generated function to pass to interceptor function above
int (*func_bambu_network_set_on_local_message_fn)(void *agent, OnMessageFn fn) = NULL;

extern "C" int bambu_network_set_on_local_message_fn(void *agent, OnMessageFn fn){
	_BSWHPLOG_VERBOSE("bambu_network_set_on_local_message_fn");

	// keep a pointer here, we're assuming the studio only expects the last set callback to be active
	_BSWHP_on_message_fn = fn;

	if ( !func_bambu_network_set_on_local_message_fn ){
		void * f = _BSWHP_get_function("bambu_network_set_on_local_message_fn");
		func_bambu_network_set_on_local_message_fn = ((int(*)(void *agent, OnMessageFn fn))f);
	}

	return func_bambu_network_set_on_local_message_fn(agent, _BSWHP_on_local_message_fn);
}












/** generated functions **/


bool (*func_bambu_network_check_debug_consistent)(bool is_debug) = NULL;

extern "C" bool bambu_network_check_debug_consistent(bool is_debug){
	_BSWHPLOG_VERBOSE("bambu_network_check_debug_consistent");

	if ( !func_bambu_network_check_debug_consistent ){
		void * f = _BSWHP_get_function("bambu_network_check_debug_consistent");
		func_bambu_network_check_debug_consistent = ((bool(*)(bool is_debug))f);
	}

	return func_bambu_network_check_debug_consistent(is_debug);
}


std::string (*func_bambu_network_get_version)() = NULL;

extern "C" std::string bambu_network_get_version(){
	_BSWHPLOG_VERBOSE("bambu_network_get_version");

	if ( !func_bambu_network_get_version ){
		void * f = _BSWHP_get_function("bambu_network_get_version");
		func_bambu_network_get_version = ((std::string(*)())f);
	}

	std::string rval = func_bambu_network_get_version();
	_BSWHPLOG("bambu_network_get_version = %s", rval.c_str());
	return rval;
}


void* (*func_bambu_network_create_agent)(std::string log_dir) = NULL;

extern "C" void* bambu_network_create_agent(std::string log_dir){
	_BSWHPLOG_VERBOSE("bambu_network_create_agent");

	if ( !func_bambu_network_create_agent ){
		void * f = _BSWHP_get_function("bambu_network_create_agent");
		func_bambu_network_create_agent = ((void*(*)(std::string log_dir))f);
	}

	return func_bambu_network_create_agent(log_dir);
}


int (*func_bambu_network_destroy_agent)(void *agent) = NULL;

extern "C" int bambu_network_destroy_agent(void *agent){
	_BSWHPLOG_VERBOSE("bambu_network_destroy_agent");

	if ( !func_bambu_network_destroy_agent ){
		void * f = _BSWHP_get_function("bambu_network_destroy_agent");
		func_bambu_network_destroy_agent = ((int(*)(void *agent))f);
	}

	return func_bambu_network_destroy_agent(agent);
}


int (*func_bambu_network_init_log)(void *agent) = NULL;

extern "C" int bambu_network_init_log(void *agent){
	_BSWHPLOG_VERBOSE("bambu_network_init_log");

	if ( !func_bambu_network_init_log ){
		void * f = _BSWHP_get_function("bambu_network_init_log");
		func_bambu_network_init_log = ((int(*)(void *agent))f);
	}

	return func_bambu_network_init_log(agent);
}


int (*func_bambu_network_set_config_dir)(void *agent, std::string config_dir) = NULL;

extern "C" int bambu_network_set_config_dir(void *agent, std::string config_dir){
	_BSWHPLOG_VERBOSE("bambu_network_set_config_dir");

	if ( !func_bambu_network_set_config_dir ){
		void * f = _BSWHP_get_function("bambu_network_set_config_dir");
		func_bambu_network_set_config_dir = ((int(*)(void *agent, std::string config_dir))f);
	}

	return func_bambu_network_set_config_dir(agent, config_dir);
}


int (*func_bambu_network_set_cert_file)(void *agent, std::string folder, std::string filename) = NULL;

extern "C" int bambu_network_set_cert_file(void *agent, std::string folder, std::string filename){
	_BSWHPLOG_VERBOSE("bambu_network_set_cert_file");

	if ( !func_bambu_network_set_cert_file ){
		void * f = _BSWHP_get_function("bambu_network_set_cert_file");
		func_bambu_network_set_cert_file = ((int(*)(void *agent, std::string folder, std::string filename))f);
	}

	return func_bambu_network_set_cert_file(agent, folder, filename);
}


int (*func_bambu_network_set_country_code)(void *agent, std::string country_code) = NULL;

extern "C" int bambu_network_set_country_code(void *agent, std::string country_code){
	_BSWHPLOG_VERBOSE("bambu_network_set_country_code");

	if ( !func_bambu_network_set_country_code ){
		void * f = _BSWHP_get_function("bambu_network_set_country_code");
		func_bambu_network_set_country_code = ((int(*)(void *agent, std::string country_code))f);
	}

	return func_bambu_network_set_country_code(agent, country_code);
}


int (*func_bambu_network_start)(void *agent) = NULL;

extern "C" int bambu_network_start(void *agent){
	_BSWHPLOG_VERBOSE("bambu_network_start");

	if ( !func_bambu_network_start ){
		void * f = _BSWHP_get_function("bambu_network_start");
		func_bambu_network_start = ((int(*)(void *agent))f);
	}

	return func_bambu_network_start(agent);
}


int (*func_bambu_network_set_on_ssdp_msg_fn)(void *agent, OnMsgArrivedFn fn) = NULL;

extern "C" int bambu_network_set_on_ssdp_msg_fn(void *agent, OnMsgArrivedFn fn){
	_BSWHPLOG_VERBOSE("bambu_network_set_on_ssdp_msg_fn");

	if ( !func_bambu_network_set_on_ssdp_msg_fn ){
		void * f = _BSWHP_get_function("bambu_network_set_on_ssdp_msg_fn");
		func_bambu_network_set_on_ssdp_msg_fn = ((int(*)(void *agent, OnMsgArrivedFn fn))f);
	}

	return func_bambu_network_set_on_ssdp_msg_fn(agent, fn);
}


int (*func_bambu_network_set_on_user_login_fn)(void *agent, OnUserLoginFn fn) = NULL;

extern "C" int bambu_network_set_on_user_login_fn(void *agent, OnUserLoginFn fn){
	_BSWHPLOG_VERBOSE("bambu_network_set_on_user_login_fn");

	if ( !func_bambu_network_set_on_user_login_fn ){
		void * f = _BSWHP_get_function("bambu_network_set_on_user_login_fn");
		func_bambu_network_set_on_user_login_fn = ((int(*)(void *agent, OnUserLoginFn fn))f);
	}

	return func_bambu_network_set_on_user_login_fn(agent, fn);
}


int (*func_bambu_network_set_on_printer_connected_fn)(void *agent, OnPrinterConnectedFn fn) = NULL;

extern "C" int bambu_network_set_on_printer_connected_fn(void *agent, OnPrinterConnectedFn fn){
	_BSWHPLOG_VERBOSE("bambu_network_set_on_printer_connected_fn");

	if ( !func_bambu_network_set_on_printer_connected_fn ){
		void * f = _BSWHP_get_function("bambu_network_set_on_printer_connected_fn");
		func_bambu_network_set_on_printer_connected_fn = ((int(*)(void *agent, OnPrinterConnectedFn fn))f);
	}

	return func_bambu_network_set_on_printer_connected_fn(agent, fn);
}


int (*func_bambu_network_set_on_server_connected_fn)(void *agent, OnServerConnectedFn fn) = NULL;

extern "C" int bambu_network_set_on_server_connected_fn(void *agent, OnServerConnectedFn fn){
	_BSWHPLOG_VERBOSE("bambu_network_set_on_server_connected_fn");

	if ( !func_bambu_network_set_on_server_connected_fn ){
		void * f = _BSWHP_get_function("bambu_network_set_on_server_connected_fn");
		func_bambu_network_set_on_server_connected_fn = ((int(*)(void *agent, OnServerConnectedFn fn))f);
	}

	return func_bambu_network_set_on_server_connected_fn(agent, fn);
}


int (*func_bambu_network_set_on_http_error_fn)(void *agent, OnHttpErrorFn fn) = NULL;

extern "C" int bambu_network_set_on_http_error_fn(void *agent, OnHttpErrorFn fn){
	_BSWHPLOG_VERBOSE("bambu_network_set_on_http_error_fn");

	if ( !func_bambu_network_set_on_http_error_fn ){
		void * f = _BSWHP_get_function("bambu_network_set_on_http_error_fn");
		func_bambu_network_set_on_http_error_fn = ((int(*)(void *agent, OnHttpErrorFn fn))f);
	}

	return func_bambu_network_set_on_http_error_fn(agent, fn);
}


int (*func_bambu_network_set_get_country_code_fn)(void *agent, GetCountryCodeFn fn) = NULL;

extern "C" int bambu_network_set_get_country_code_fn(void *agent, GetCountryCodeFn fn){
	_BSWHPLOG_VERBOSE("bambu_network_set_get_country_code_fn");

	if ( !func_bambu_network_set_get_country_code_fn ){
		void * f = _BSWHP_get_function("bambu_network_set_get_country_code_fn");
		func_bambu_network_set_get_country_code_fn = ((int(*)(void *agent, GetCountryCodeFn fn))f);
	}

	return func_bambu_network_set_get_country_code_fn(agent, fn);
}


int (*func_bambu_network_set_on_subscribe_failure_fn)(void *agent, GetSubscribeFailureFn fn) = NULL;

extern "C" int bambu_network_set_on_subscribe_failure_fn(void *agent, GetSubscribeFailureFn fn){
	_BSWHPLOG_VERBOSE("bambu_network_set_on_subscribe_failure_fn");

	if ( !func_bambu_network_set_on_subscribe_failure_fn ){
		void * f = _BSWHP_get_function("bambu_network_set_on_subscribe_failure_fn");
		func_bambu_network_set_on_subscribe_failure_fn = ((int(*)(void *agent, GetSubscribeFailureFn fn))f);
	}

	return func_bambu_network_set_on_subscribe_failure_fn(agent, fn);
}


int (*func_bambu_network_set_on_message_fn)(void *agent, OnMessageFn fn) = NULL;

extern "C" int bambu_network_set_on_message_fn(void *agent, OnMessageFn fn){
	_BSWHPLOG_VERBOSE("bambu_network_set_on_message_fn");

	if ( !func_bambu_network_set_on_message_fn ){
		void * f = _BSWHP_get_function("bambu_network_set_on_message_fn");
		func_bambu_network_set_on_message_fn = ((int(*)(void *agent, OnMessageFn fn))f);
	}



	return func_bambu_network_set_on_message_fn(agent, fn);
}


int (*func_bambu_network_set_on_local_connect_fn)(void *agent, OnLocalConnectedFn fn) = NULL;

extern "C" int bambu_network_set_on_local_connect_fn(void *agent, OnLocalConnectedFn fn){
	_BSWHPLOG_VERBOSE("bambu_network_set_on_local_connect_fn");

	if ( !func_bambu_network_set_on_local_connect_fn ){
		void * f = _BSWHP_get_function("bambu_network_set_on_local_connect_fn");
		func_bambu_network_set_on_local_connect_fn = ((int(*)(void *agent, OnLocalConnectedFn fn))f);
	}

	return func_bambu_network_set_on_local_connect_fn(agent, fn);
}


int (*func_bambu_network_set_queue_on_main_fn)(void *agent, QueueOnMainFn fn) = NULL;

extern "C" int bambu_network_set_queue_on_main_fn(void *agent, QueueOnMainFn fn){
	_BSWHPLOG_VERBOSE("bambu_network_set_queue_on_main_fn");

	if ( !func_bambu_network_set_queue_on_main_fn ){
		void * f = _BSWHP_get_function("bambu_network_set_queue_on_main_fn");
		func_bambu_network_set_queue_on_main_fn = ((int(*)(void *agent, QueueOnMainFn fn))f);
	}

	return func_bambu_network_set_queue_on_main_fn(agent, fn);
}


int (*func_bambu_network_connect_server)(void *agent) = NULL;

extern "C" int bambu_network_connect_server(void *agent){
	_BSWHPLOG_VERBOSE("bambu_network_connect_server");

	if ( !func_bambu_network_connect_server ){
		void * f = _BSWHP_get_function("bambu_network_connect_server");
		func_bambu_network_connect_server = ((int(*)(void *agent))f);
	}

	return func_bambu_network_connect_server(agent);
}


bool (*func_bambu_network_is_server_connected)(void *agent) = NULL;

extern "C" bool bambu_network_is_server_connected(void *agent){
	_BSWHPLOG_VERBOSE("bambu_network_is_server_connected");

	if ( !func_bambu_network_is_server_connected ){
		void * f = _BSWHP_get_function("bambu_network_is_server_connected");
		func_bambu_network_is_server_connected = ((bool(*)(void *agent))f);
	}

	return func_bambu_network_is_server_connected(agent);
}


int (*func_bambu_network_refresh_connection)(void *agent) = NULL;

extern "C" int bambu_network_refresh_connection(void *agent){
	_BSWHPLOG_VERBOSE("bambu_network_refresh_connection");

	if ( !func_bambu_network_refresh_connection ){
		void * f = _BSWHP_get_function("bambu_network_refresh_connection");
		func_bambu_network_refresh_connection = ((int(*)(void *agent))f);
	}

	return func_bambu_network_refresh_connection(agent);
}


int (*func_bambu_network_start_subscribe)(void *agent, std::string module) = NULL;

extern "C" int bambu_network_start_subscribe(void *agent, std::string module){
	_BSWHPLOG_VERBOSE("bambu_network_start_subscribe");

	if ( !func_bambu_network_start_subscribe ){
		void * f = _BSWHP_get_function("bambu_network_start_subscribe");
		func_bambu_network_start_subscribe = ((int(*)(void *agent, std::string module))f);
	}

	return func_bambu_network_start_subscribe(agent, module);
}


int (*func_bambu_network_stop_subscribe)(void *agent, std::string module) = NULL;

extern "C" int bambu_network_stop_subscribe(void *agent, std::string module){
	_BSWHPLOG_VERBOSE("bambu_network_stop_subscribe");

	if ( !func_bambu_network_stop_subscribe ){
		void * f = _BSWHP_get_function("bambu_network_stop_subscribe");
		func_bambu_network_stop_subscribe = ((int(*)(void *agent, std::string module))f);
	}

	return func_bambu_network_stop_subscribe(agent, module);
}


int (*func_bambu_network_send_message)(void *agent, std::string dev_id, std::string json_str, int qos) = NULL;

extern "C" int bambu_network_send_message(void *agent, std::string dev_id, std::string json_str, int qos){
	_BSWHPLOG_VERBOSE("bambu_network_send_message");

	if ( !func_bambu_network_send_message ){
		void * f = _BSWHP_get_function("bambu_network_send_message");
		func_bambu_network_send_message = ((int(*)(void *agent, std::string dev_id, std::string json_str, int qos))f);
	}

	return func_bambu_network_send_message(agent, dev_id, json_str, qos);
}


int (*func_bambu_network_connect_printer)(void *agent, std::string dev_id, std::string dev_ip, std::string username, std::string password, bool use_ssl) = NULL;

extern "C" int bambu_network_connect_printer(void *agent, std::string dev_id, std::string dev_ip, std::string username, std::string password, bool use_ssl){
	_BSWHPLOG_VERBOSE("bambu_network_connect_printer");

	if ( !func_bambu_network_connect_printer ){
		void * f = _BSWHP_get_function("bambu_network_connect_printer");
		func_bambu_network_connect_printer = ((int(*)(void *agent, std::string dev_id, std::string dev_ip, std::string username, std::string password, bool use_ssl))f);
	}

	return func_bambu_network_connect_printer(agent, dev_id, dev_ip, username, password, use_ssl);
}


int (*func_bambu_network_disconnect_printer)(void *agent) = NULL;

extern "C" int bambu_network_disconnect_printer(void *agent){
	_BSWHPLOG_VERBOSE("bambu_network_disconnect_printer");

	if ( !func_bambu_network_disconnect_printer ){
		void * f = _BSWHP_get_function("bambu_network_disconnect_printer");
		func_bambu_network_disconnect_printer = ((int(*)(void *agent))f);
	}

	return func_bambu_network_disconnect_printer(agent);
}


int (*func_bambu_network_send_message_to_printer)(void *agent, std::string dev_id, std::string json_str, int qos) = NULL;

extern "C" int bambu_network_send_message_to_printer(void *agent, std::string dev_id, std::string json_str, int qos){
	_BSWHPLOG_VERBOSE("bambu_network_send_message_to_printer");

	if ( !func_bambu_network_send_message_to_printer ){
		void * f = _BSWHP_get_function("bambu_network_send_message_to_printer");
		func_bambu_network_send_message_to_printer = ((int(*)(void *agent, std::string dev_id, std::string json_str, int qos))f);
	}

	return func_bambu_network_send_message_to_printer(agent, dev_id, json_str, qos);
}


bool (*func_bambu_network_start_discovery)(void *agent, bool start, bool sending) = NULL;

extern "C" bool bambu_network_start_discovery(void *agent, bool start, bool sending){
	_BSWHPLOG_VERBOSE("bambu_network_start_discovery");

	if ( !func_bambu_network_start_discovery ){
		void * f = _BSWHP_get_function("bambu_network_start_discovery");
		func_bambu_network_start_discovery = ((bool(*)(void *agent, bool start, bool sending))f);
	}

	return func_bambu_network_start_discovery(agent, start, sending);
}


int (*func_bambu_network_change_user)(void *agent, std::string user_info) = NULL;

extern "C" int bambu_network_change_user(void *agent, std::string user_info){
	_BSWHPLOG_VERBOSE("bambu_network_change_user");

	if ( !func_bambu_network_change_user ){
		void * f = _BSWHP_get_function("bambu_network_change_user");
		func_bambu_network_change_user = ((int(*)(void *agent, std::string user_info))f);
	}

	return func_bambu_network_change_user(agent, user_info);
}


bool (*func_bambu_network_is_user_login)(void *agent) = NULL;

extern "C" bool bambu_network_is_user_login(void *agent){
	_BSWHPLOG_VERBOSE("bambu_network_is_user_login");

	if ( !func_bambu_network_is_user_login ){
		void * f = _BSWHP_get_function("bambu_network_is_user_login");
		func_bambu_network_is_user_login = ((bool(*)(void *agent))f);
	}

	return func_bambu_network_is_user_login(agent);
}


int (*func_bambu_network_user_logout)(void *agent) = NULL;

extern "C" int bambu_network_user_logout(void *agent){
	_BSWHPLOG_VERBOSE("bambu_network_user_logout");

	if ( !func_bambu_network_user_logout ){
		void * f = _BSWHP_get_function("bambu_network_user_logout");
		func_bambu_network_user_logout = ((int(*)(void *agent))f);
	}

	return func_bambu_network_user_logout(agent);
}


std::string (*func_bambu_network_get_user_id)(void *agent) = NULL;

extern "C" std::string bambu_network_get_user_id(void *agent){
	_BSWHPLOG_VERBOSE("bambu_network_get_user_id");

	if ( !func_bambu_network_get_user_id ){
		void * f = _BSWHP_get_function("bambu_network_get_user_id");
		func_bambu_network_get_user_id = ((std::string(*)(void *agent))f);
	}

	return func_bambu_network_get_user_id(agent);
}


std::string (*func_bambu_network_get_user_name)(void *agent) = NULL;

extern "C" std::string bambu_network_get_user_name(void *agent){
	_BSWHPLOG_VERBOSE("bambu_network_get_user_name");

	if ( !func_bambu_network_get_user_name ){
		void * f = _BSWHP_get_function("bambu_network_get_user_name");
		func_bambu_network_get_user_name = ((std::string(*)(void *agent))f);
	}

	return func_bambu_network_get_user_name(agent);
}


std::string (*func_bambu_network_get_user_avatar)(void *agent) = NULL;

extern "C" std::string bambu_network_get_user_avatar(void *agent){
	_BSWHPLOG_VERBOSE("bambu_network_get_user_avatar");

	if ( !func_bambu_network_get_user_avatar ){
		void * f = _BSWHP_get_function("bambu_network_get_user_avatar");
		func_bambu_network_get_user_avatar = ((std::string(*)(void *agent))f);
	}

	return func_bambu_network_get_user_avatar(agent);
}


std::string (*func_bambu_network_get_user_nickanme)(void *agent) = NULL;

extern "C" std::string bambu_network_get_user_nickanme(void *agent){
	_BSWHPLOG_VERBOSE("bambu_network_get_user_nickanme");

	if ( !func_bambu_network_get_user_nickanme ){
		void * f = _BSWHP_get_function("bambu_network_get_user_nickanme");
		func_bambu_network_get_user_nickanme = ((std::string(*)(void *agent))f);
	}

	return func_bambu_network_get_user_nickanme(agent);
}


std::string (*func_bambu_network_build_login_cmd)(void *agent) = NULL;

extern "C" std::string bambu_network_build_login_cmd(void *agent){
	_BSWHPLOG_VERBOSE("bambu_network_build_login_cmd");

	if ( !func_bambu_network_build_login_cmd ){
		void * f = _BSWHP_get_function("bambu_network_build_login_cmd");
		func_bambu_network_build_login_cmd = ((std::string(*)(void *agent))f);
	}

	return func_bambu_network_build_login_cmd(agent);
}


std::string (*func_bambu_network_build_logout_cmd)(void *agent) = NULL;

extern "C" std::string bambu_network_build_logout_cmd(void *agent){
	_BSWHPLOG_VERBOSE("bambu_network_build_logout_cmd");

	if ( !func_bambu_network_build_logout_cmd ){
		void * f = _BSWHP_get_function("bambu_network_build_logout_cmd");
		func_bambu_network_build_logout_cmd = ((std::string(*)(void *agent))f);
	}

	return func_bambu_network_build_logout_cmd(agent);
}


std::string (*func_bambu_network_build_login_info)(void *agent) = NULL;

extern "C" std::string bambu_network_build_login_info(void *agent){
	_BSWHPLOG_VERBOSE("bambu_network_build_login_info");

	if ( !func_bambu_network_build_login_info ){
		void * f = _BSWHP_get_function("bambu_network_build_login_info");
		func_bambu_network_build_login_info = ((std::string(*)(void *agent))f);
	}

	return func_bambu_network_build_login_info(agent);
}


int (*func_bambu_network_bind)(void *agent, std::string dev_ip, std::string dev_id, std::string sec_link, std::string timezone, bool improved, OnUpdateStatusFn update_fn) = NULL;

extern "C" int bambu_network_bind(void *agent, std::string dev_ip, std::string dev_id, std::string sec_link, std::string timezone, bool improved, OnUpdateStatusFn update_fn){
	_BSWHPLOG_VERBOSE("bambu_network_bind");

	if ( !func_bambu_network_bind ){
		void * f = _BSWHP_get_function("bambu_network_bind");
		func_bambu_network_bind = ((int(*)(void *agent, std::string dev_ip, std::string dev_id, std::string sec_link, std::string timezone, bool improved, OnUpdateStatusFn update_fn))f);
	}

	return func_bambu_network_bind(agent, dev_ip, dev_id, sec_link, timezone, improved, update_fn);
}


int (*func_bambu_network_unbind)(void *agent, std::string dev_id) = NULL;

extern "C" int bambu_network_unbind(void *agent, std::string dev_id){
	_BSWHPLOG_VERBOSE("bambu_network_unbind");

	if ( !func_bambu_network_unbind ){
		void * f = _BSWHP_get_function("bambu_network_unbind");
		func_bambu_network_unbind = ((int(*)(void *agent, std::string dev_id))f);
	}

	return func_bambu_network_unbind(agent, dev_id);
}


std::string (*func_bambu_network_get_bambulab_host)(void *agent) = NULL;

extern "C" std::string bambu_network_get_bambulab_host(void *agent){
	_BSWHPLOG_VERBOSE("bambu_network_get_bambulab_host");

	if ( !func_bambu_network_get_bambulab_host ){
		void * f = _BSWHP_get_function("bambu_network_get_bambulab_host");
		func_bambu_network_get_bambulab_host = ((std::string(*)(void *agent))f);
	}

	return func_bambu_network_get_bambulab_host(agent);
}


std::string (*func_bambu_network_get_user_selected_machine)(void *agent) = NULL;

extern "C" std::string bambu_network_get_user_selected_machine(void *agent){
	_BSWHPLOG_VERBOSE("bambu_network_get_user_selected_machine");

	if ( !func_bambu_network_get_user_selected_machine ){
		void * f = _BSWHP_get_function("bambu_network_get_user_selected_machine");
		func_bambu_network_get_user_selected_machine = ((std::string(*)(void *agent))f);
	}

	return func_bambu_network_get_user_selected_machine(agent);
}


int (*func_bambu_network_set_user_selected_machine)(void *agent, std::string dev_id) = NULL;

extern "C" int bambu_network_set_user_selected_machine(void *agent, std::string dev_id){
	_BSWHPLOG_VERBOSE("bambu_network_set_user_selected_machine");

	if ( !func_bambu_network_set_user_selected_machine ){
		void * f = _BSWHP_get_function("bambu_network_set_user_selected_machine");
		func_bambu_network_set_user_selected_machine = ((int(*)(void *agent, std::string dev_id))f);
	}

	return func_bambu_network_set_user_selected_machine(agent, dev_id);
}


int (*func_bambu_network_start_print)(void *agent, PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn) = NULL;

extern "C" int bambu_network_start_print(void *agent, PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn){
	_BSWHPLOG_VERBOSE("bambu_network_start_print");

	if ( !func_bambu_network_start_print ){
		void * f = _BSWHP_get_function("bambu_network_start_print");
		func_bambu_network_start_print = ((int(*)(void *agent, PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn))f);
	}

	return func_bambu_network_start_print(agent, params, update_fn, cancel_fn, wait_fn);
}


int (*func_bambu_network_start_local_print_with_record)(void *agent, PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn) = NULL;

extern "C" int bambu_network_start_local_print_with_record(void *agent, PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn){
	_BSWHPLOG_VERBOSE("bambu_network_start_local_print_with_record");

	if ( !func_bambu_network_start_local_print_with_record ){
		void * f = _BSWHP_get_function("bambu_network_start_local_print_with_record");
		func_bambu_network_start_local_print_with_record = ((int(*)(void *agent, PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn))f);
	}

	return func_bambu_network_start_local_print_with_record(agent, params, update_fn, cancel_fn, wait_fn);
}


int (*func_bambu_network_start_send_gcode_to_sdcard)(void *agent, PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn) = NULL;

extern "C" int bambu_network_start_send_gcode_to_sdcard(void *agent, PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn){
	_BSWHPLOG_VERBOSE("bambu_network_start_send_gcode_to_sdcard");

	if ( !func_bambu_network_start_send_gcode_to_sdcard ){
		void * f = _BSWHP_get_function("bambu_network_start_send_gcode_to_sdcard");
		func_bambu_network_start_send_gcode_to_sdcard = ((int(*)(void *agent, PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn))f);
	}

	return func_bambu_network_start_send_gcode_to_sdcard(agent, params, update_fn, cancel_fn, wait_fn);
}


int (*func_bambu_network_start_local_print)(void *agent, PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn) = NULL;

extern "C" int bambu_network_start_local_print(void *agent, PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn){
	_BSWHPLOG_VERBOSE("bambu_network_start_local_print");

	if ( !func_bambu_network_start_local_print ){
		void * f = _BSWHP_get_function("bambu_network_start_local_print");
		func_bambu_network_start_local_print = ((int(*)(void *agent, PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn))f);
	}

	return func_bambu_network_start_local_print(agent, params, update_fn, cancel_fn);
}


int (*func_bambu_network_get_user_presets)(void *agent, std::map<std::string, std::map<std::string, std::string>>* user_presets) = NULL;

extern "C" int bambu_network_get_user_presets(void *agent, std::map<std::string, std::map<std::string, std::string>>* user_presets){
	_BSWHPLOG_VERBOSE("bambu_network_get_user_presets");

	if ( !func_bambu_network_get_user_presets ){
		void * f = _BSWHP_get_function("bambu_network_get_user_presets");
		func_bambu_network_get_user_presets = ((int(*)(void *agent, std::map<std::string, std::map<std::string, std::string>>* user_presets))f);
	}

	return func_bambu_network_get_user_presets(agent, user_presets);
}


std::string (*func_bambu_network_request_setting_id)(void *agent, std::string name, std::map<std::string, std::string>* values_map, unsigned int* http_code) = NULL;

extern "C" std::string bambu_network_request_setting_id(void *agent, std::string name, std::map<std::string, std::string>* values_map, unsigned int* http_code){
	_BSWHPLOG_VERBOSE("bambu_network_request_setting_id");

	if ( !func_bambu_network_request_setting_id ){
		void * f = _BSWHP_get_function("bambu_network_request_setting_id");
		func_bambu_network_request_setting_id = ((std::string(*)(void *agent, std::string name, std::map<std::string, std::string>* values_map, unsigned int* http_code))f);
	}

	return func_bambu_network_request_setting_id(agent, name, values_map, http_code);
}


int (*func_bambu_network_put_setting)(void *agent, std::string setting_id, std::string name, std::map<std::string, std::string>* values_map, unsigned int* http_code) = NULL;

extern "C" int bambu_network_put_setting(void *agent, std::string setting_id, std::string name, std::map<std::string, std::string>* values_map, unsigned int* http_code){
	_BSWHPLOG_VERBOSE("bambu_network_put_setting");

	if ( !func_bambu_network_put_setting ){
		void * f = _BSWHP_get_function("bambu_network_put_setting");
		func_bambu_network_put_setting = ((int(*)(void *agent, std::string setting_id, std::string name, std::map<std::string, std::string>* values_map, unsigned int* http_code))f);
	}

	return func_bambu_network_put_setting(agent, setting_id, name, values_map, http_code);
}


int (*func_bambu_network_get_setting_list)(void *agent, std::string bundle_version, ProgressFn pro_fn, WasCancelledFn cancel_fn) = NULL;

extern "C" int bambu_network_get_setting_list(void *agent, std::string bundle_version, ProgressFn pro_fn, WasCancelledFn cancel_fn){
	_BSWHPLOG_VERBOSE("bambu_network_get_setting_list");

	if ( !func_bambu_network_get_setting_list ){
		void * f = _BSWHP_get_function("bambu_network_get_setting_list");
		func_bambu_network_get_setting_list = ((int(*)(void *agent, std::string bundle_version, ProgressFn pro_fn, WasCancelledFn cancel_fn))f);
	}

	return func_bambu_network_get_setting_list(agent, bundle_version, pro_fn, cancel_fn);
}


int (*func_bambu_network_get_setting_list2)(void* agent, std::string bundle_version, CheckFn chk_fn, ProgressFn pro_fn, WasCancelledFn cancel_fn) = NULL;

extern "C" int bambu_network_get_setting_list2(void* agent, std::string bundle_version, CheckFn chk_fn, ProgressFn pro_fn, WasCancelledFn cancel_fn){
	_BSWHPLOG_VERBOSE("bambu_network_get_setting_list2");

	if ( !func_bambu_network_get_setting_list2 ){
		void * f = _BSWHP_get_function("bambu_network_get_setting_list2");
		func_bambu_network_get_setting_list2 = ((int(*)(void* agent, std::string bundle_version, CheckFn chk_fn, ProgressFn pro_fn, WasCancelledFn cancel_fn))f);
	}

	return func_bambu_network_get_setting_list2(agent, bundle_version, chk_fn, pro_fn, cancel_fn);
}


int (*func_bambu_network_delete_setting)(void *agent, std::string setting_id) = NULL;

extern "C" int bambu_network_delete_setting(void *agent, std::string setting_id){
	_BSWHPLOG_VERBOSE("bambu_network_delete_setting");

	if ( !func_bambu_network_delete_setting ){
		void * f = _BSWHP_get_function("bambu_network_delete_setting");
		func_bambu_network_delete_setting = ((int(*)(void *agent, std::string setting_id))f);
	}

	return func_bambu_network_delete_setting(agent, setting_id);
}


std::string (*func_bambu_network_get_studio_info_url)(void *agent) = NULL;

extern "C" std::string bambu_network_get_studio_info_url(void *agent){
	_BSWHPLOG_VERBOSE("bambu_network_get_studio_info_url");

	if ( !func_bambu_network_get_studio_info_url ){
		void * f = _BSWHP_get_function("bambu_network_get_studio_info_url");
		func_bambu_network_get_studio_info_url = ((std::string(*)(void *agent))f);
	}

	return func_bambu_network_get_studio_info_url(agent);
}


int (*func_bambu_network_set_extra_http_header)(void *agent, std::map<std::string, std::string> extra_headers) = NULL;

extern "C" int bambu_network_set_extra_http_header(void *agent, std::map<std::string, std::string> extra_headers){
	_BSWHPLOG_VERBOSE("bambu_network_set_extra_http_header");

	if ( !func_bambu_network_set_extra_http_header ){
		void * f = _BSWHP_get_function("bambu_network_set_extra_http_header");
		func_bambu_network_set_extra_http_header = ((int(*)(void *agent, std::map<std::string, std::string> extra_headers))f);
	}

	return func_bambu_network_set_extra_http_header(agent, extra_headers);
}


int (*func_bambu_network_get_my_message)(void *agent, int type, int after, int limit, unsigned int* http_code, std::string* http_body) = NULL;

extern "C" int bambu_network_get_my_message(void *agent, int type, int after, int limit, unsigned int* http_code, std::string* http_body){
	_BSWHPLOG_VERBOSE("bambu_network_get_my_message");

	if ( !func_bambu_network_get_my_message ){
		void * f = _BSWHP_get_function("bambu_network_get_my_message");
		func_bambu_network_get_my_message = ((int(*)(void *agent, int type, int after, int limit, unsigned int* http_code, std::string* http_body))f);
	}

	return func_bambu_network_get_my_message(agent, type, after, limit, http_code, http_body);
}


int (*func_bambu_network_check_user_task_report)(void *agent, int* task_id, bool* printable) = NULL;

extern "C" int bambu_network_check_user_task_report(void *agent, int* task_id, bool* printable){
	_BSWHPLOG_VERBOSE("bambu_network_check_user_task_report");

	if ( !func_bambu_network_check_user_task_report ){
		void * f = _BSWHP_get_function("bambu_network_check_user_task_report");
		func_bambu_network_check_user_task_report = ((int(*)(void *agent, int* task_id, bool* printable))f);
	}

	return func_bambu_network_check_user_task_report(agent, task_id, printable);
}


int (*func_bambu_network_get_user_print_info)(void *agent, unsigned int* http_code, std::string* http_body) = NULL;

extern "C" int bambu_network_get_user_print_info(void *agent, unsigned int* http_code, std::string* http_body){
	_BSWHPLOG_VERBOSE("bambu_network_get_user_print_info");

	if ( !func_bambu_network_get_user_print_info ){
		void * f = _BSWHP_get_function("bambu_network_get_user_print_info");
		func_bambu_network_get_user_print_info = ((int(*)(void *agent, unsigned int* http_code, std::string* http_body))f);
	}

	return func_bambu_network_get_user_print_info(agent, http_code, http_body);
}


int (*func_bambu_network_get_printer_firmware)(void *agent, std::string dev_id, unsigned* http_code, std::string* http_body) = NULL;

extern "C" int bambu_network_get_printer_firmware(void *agent, std::string dev_id, unsigned* http_code, std::string* http_body){
	_BSWHPLOG_VERBOSE("bambu_network_get_printer_firmware");

	if ( !func_bambu_network_get_printer_firmware ){
		void * f = _BSWHP_get_function("bambu_network_get_printer_firmware");
		func_bambu_network_get_printer_firmware = ((int(*)(void *agent, std::string dev_id, unsigned* http_code, std::string* http_body))f);
	}

	return func_bambu_network_get_printer_firmware(agent, dev_id, http_code, http_body);
}


int (*func_bambu_network_get_task_plate_index)(void *agent, std::string task_id, int* plate_index) = NULL;

extern "C" int bambu_network_get_task_plate_index(void *agent, std::string task_id, int* plate_index){
	_BSWHPLOG_VERBOSE("bambu_network_get_task_plate_index");

	if ( !func_bambu_network_get_task_plate_index ){
		void * f = _BSWHP_get_function("bambu_network_get_task_plate_index");
		func_bambu_network_get_task_plate_index = ((int(*)(void *agent, std::string task_id, int* plate_index))f);
	}

	return func_bambu_network_get_task_plate_index(agent, task_id, plate_index);
}


int (*func_bambu_network_get_user_info)(void *agent, int* identifier) = NULL;

extern "C" int bambu_network_get_user_info(void *agent, int* identifier){
	_BSWHPLOG_VERBOSE("bambu_network_get_user_info");

	if ( !func_bambu_network_get_user_info ){
		void * f = _BSWHP_get_function("bambu_network_get_user_info");
		func_bambu_network_get_user_info = ((int(*)(void *agent, int* identifier))f);
	}

	return func_bambu_network_get_user_info(agent, identifier);
}


int (*func_bambu_network_request_bind_ticket)(void *agent, std::string* ticket) = NULL;

extern "C" int bambu_network_request_bind_ticket(void *agent, std::string* ticket){
	_BSWHPLOG_VERBOSE("bambu_network_request_bind_ticket");

	if ( !func_bambu_network_request_bind_ticket ){
		void * f = _BSWHP_get_function("bambu_network_request_bind_ticket");
		func_bambu_network_request_bind_ticket = ((int(*)(void *agent, std::string* ticket))f);
	}

	return func_bambu_network_request_bind_ticket(agent, ticket);
}


int (*func_bambu_network_get_subtask_info)(void *agent, std::string subtask_id, std::string* task_json, unsigned int* http_code, std::string* http_body) = NULL;

extern "C" int bambu_network_get_subtask_info(void *agent, std::string subtask_id, std::string* task_json, unsigned int* http_code, std::string* http_body){
	_BSWHPLOG_VERBOSE("bambu_network_get_subtask_info");

	if ( !func_bambu_network_get_subtask_info ){
		void * f = _BSWHP_get_function("bambu_network_get_subtask_info");
		func_bambu_network_get_subtask_info = ((int(*)(void *agent, std::string subtask_id, std::string* task_json, unsigned int* http_code, std::string* http_body))f);
	}

	return func_bambu_network_get_subtask_info(agent, subtask_id, task_json, http_code, http_body);
}


int (*func_bambu_network_get_slice_info)(void *agent, std::string project_id, std::string profile_id, int plate_index, std::string* slice_json) = NULL;

extern "C" int bambu_network_get_slice_info(void *agent, std::string project_id, std::string profile_id, int plate_index, std::string* slice_json){
	_BSWHPLOG_VERBOSE("bambu_network_get_slice_info");

	if ( !func_bambu_network_get_slice_info ){
		void * f = _BSWHP_get_function("bambu_network_get_slice_info");
		func_bambu_network_get_slice_info = ((int(*)(void *agent, std::string project_id, std::string profile_id, int plate_index, std::string* slice_json))f);
	}

	return func_bambu_network_get_slice_info(agent, project_id, profile_id, plate_index, slice_json);
}


int (*func_bambu_network_query_bind_status)(void *agent, std::vector<std::string> query_list, unsigned int* http_code, std::string* http_body) = NULL;

extern "C" int bambu_network_query_bind_status(void *agent, std::vector<std::string> query_list, unsigned int* http_code, std::string* http_body){
	_BSWHPLOG_VERBOSE("bambu_network_query_bind_status");

	if ( !func_bambu_network_query_bind_status ){
		void * f = _BSWHP_get_function("bambu_network_query_bind_status");
		func_bambu_network_query_bind_status = ((int(*)(void *agent, std::vector<std::string> query_list, unsigned int* http_code, std::string* http_body))f);
	}

	return func_bambu_network_query_bind_status(agent, query_list, http_code, http_body);
}


int (*func_bambu_network_modify_printer_name)(void *agent, std::string dev_id, std::string dev_name) = NULL;

extern "C" int bambu_network_modify_printer_name(void *agent, std::string dev_id, std::string dev_name){
	_BSWHPLOG_VERBOSE("bambu_network_modify_printer_name");

	if ( !func_bambu_network_modify_printer_name ){
		void * f = _BSWHP_get_function("bambu_network_modify_printer_name");
		func_bambu_network_modify_printer_name = ((int(*)(void *agent, std::string dev_id, std::string dev_name))f);
	}

	return func_bambu_network_modify_printer_name(agent, dev_id, dev_name);
}


int (*func_bambu_network_get_camera_url)(void *agent, std::string dev_id, std::function<void(std::string)> callback) = NULL;

extern "C" int bambu_network_get_camera_url(void *agent, std::string dev_id, std::function<void(std::string)> callback){
	_BSWHPLOG_VERBOSE("bambu_network_get_camera_url");

	if ( !func_bambu_network_get_camera_url ){
		void * f = _BSWHP_get_function("bambu_network_get_camera_url");
		func_bambu_network_get_camera_url = ((int(*)(void *agent, std::string dev_id, std::function<void(std::string)> callback))f);
	}

	return func_bambu_network_get_camera_url(agent, dev_id, callback);
}


int (*func_bambu_network_get_design_staffpick)(void *agent, int offset, int limit, std::function<void(std::string)> callback) = NULL;

extern "C" int bambu_network_get_design_staffpick(void *agent, int offset, int limit, std::function<void(std::string)> callback){
	_BSWHPLOG_VERBOSE("bambu_network_get_design_staffpick");

	if ( !func_bambu_network_get_design_staffpick ){
		void * f = _BSWHP_get_function("bambu_network_get_design_staffpick");
		func_bambu_network_get_design_staffpick = ((int(*)(void *agent, int offset, int limit, std::function<void(std::string)> callback))f);
	}

	return func_bambu_network_get_design_staffpick(agent, offset, limit, callback);
}


int (*func_bambu_network_start_publish)(void *agent, PublishParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, std::string* out) = NULL;

extern "C" int bambu_network_start_publish(void *agent, PublishParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, std::string* out){
	_BSWHPLOG_VERBOSE("bambu_network_start_publish");

	if ( !func_bambu_network_start_publish ){
		void * f = _BSWHP_get_function("bambu_network_start_publish");
		func_bambu_network_start_publish = ((int(*)(void *agent, PublishParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, std::string* out))f);
	}

	return func_bambu_network_start_publish(agent, params, update_fn, cancel_fn, out);
}


int (*func_bambu_network_get_profile_3mf)(void *agent, BBLProfile* profile) = NULL;

extern "C" int bambu_network_get_profile_3mf(void *agent, BBLProfile* profile){
	_BSWHPLOG_VERBOSE("bambu_network_get_profile_3mf");

	if ( !func_bambu_network_get_profile_3mf ){
		void * f = _BSWHP_get_function("bambu_network_get_profile_3mf");
		func_bambu_network_get_profile_3mf = ((int(*)(void *agent, BBLProfile* profile))f);
	}

	return func_bambu_network_get_profile_3mf(agent, profile);
}


int (*func_bambu_network_get_model_publish_url)(void *agent, std::string* url) = NULL;

extern "C" int bambu_network_get_model_publish_url(void *agent, std::string* url){
	_BSWHPLOG_VERBOSE("bambu_network_get_model_publish_url");

	if ( !func_bambu_network_get_model_publish_url ){
		void * f = _BSWHP_get_function("bambu_network_get_model_publish_url");
		func_bambu_network_get_model_publish_url = ((int(*)(void *agent, std::string* url))f);
	}

	return func_bambu_network_get_model_publish_url(agent, url);
}


int (*func_bambu_network_get_subtask)(void *agent, BBLModelTask* task, OnGetSubTaskFn getsub_fn) = NULL;

extern "C" int bambu_network_get_subtask(void *agent, BBLModelTask* task, OnGetSubTaskFn getsub_fn){
	_BSWHPLOG_VERBOSE("bambu_network_get_subtask");

	if ( !func_bambu_network_get_subtask ){
		void * f = _BSWHP_get_function("bambu_network_get_subtask");
		func_bambu_network_get_subtask = ((int(*)(void *agent, BBLModelTask* task, OnGetSubTaskFn getsub_fn))f);
	}

	return func_bambu_network_get_subtask(agent, task, getsub_fn);
}


int (*func_bambu_network_get_model_mall_home_url)(void *agent, std::string* url) = NULL;

extern "C" int bambu_network_get_model_mall_home_url(void *agent, std::string* url){
	_BSWHPLOG_VERBOSE("bambu_network_get_model_mall_home_url");

	if ( !func_bambu_network_get_model_mall_home_url ){
		void * f = _BSWHP_get_function("bambu_network_get_model_mall_home_url");
		func_bambu_network_get_model_mall_home_url = ((int(*)(void *agent, std::string* url))f);
	}

	return func_bambu_network_get_model_mall_home_url(agent, url);
}


int (*func_bambu_network_get_model_mall_detail_url)(void *agent, std::string* url, std::string id) = NULL;

extern "C" int bambu_network_get_model_mall_detail_url(void *agent, std::string* url, std::string id){
	_BSWHPLOG_VERBOSE("bambu_network_get_model_mall_detail_url");

	if ( !func_bambu_network_get_model_mall_detail_url ){
		void * f = _BSWHP_get_function("bambu_network_get_model_mall_detail_url");
		func_bambu_network_get_model_mall_detail_url = ((int(*)(void *agent, std::string* url, std::string id))f);
	}

	return func_bambu_network_get_model_mall_detail_url(agent, url, id);
}


int (*func_bambu_network_get_my_profile)(void *agent, std::string token, unsigned int* http_code, std::string* http_body) = NULL;

extern "C" int bambu_network_get_my_profile(void *agent, std::string token, unsigned int* http_code, std::string* http_body){
	_BSWHPLOG_VERBOSE("bambu_network_get_my_profile");

	if ( !func_bambu_network_get_my_profile ){
		void * f = _BSWHP_get_function("bambu_network_get_my_profile");
		func_bambu_network_get_my_profile = ((int(*)(void *agent, std::string token, unsigned int* http_code, std::string* http_body))f);
	}

	return func_bambu_network_get_my_profile(agent, token, http_code, http_body);
}


int (*func_bambu_network_track_enable)(void *agent, bool enable) = NULL;

extern "C" int bambu_network_track_enable(void *agent, bool enable){
	_BSWHPLOG_VERBOSE("bambu_network_track_enable");

	if ( !func_bambu_network_track_enable ){
		void * f = _BSWHP_get_function("bambu_network_track_enable");
		func_bambu_network_track_enable = ((int(*)(void *agent, bool enable))f);
	}

	return func_bambu_network_track_enable(agent, enable);
}


int (*func_bambu_network_track_event)(void *agent, std::string evt_key, std::string content) = NULL;

extern "C" int bambu_network_track_event(void *agent, std::string evt_key, std::string content){
	_BSWHPLOG_VERBOSE("bambu_network_track_event");

	if ( !func_bambu_network_track_event ){
		void * f = _BSWHP_get_function("bambu_network_track_event");
		func_bambu_network_track_event = ((int(*)(void *agent, std::string evt_key, std::string content))f);
	}

	return func_bambu_network_track_event(agent, evt_key, content);
}


int (*func_bambu_network_track_header)(void *agent, std::string header) = NULL;

extern "C" int bambu_network_track_header(void *agent, std::string header){
	_BSWHPLOG_VERBOSE("bambu_network_track_header");

	if ( !func_bambu_network_track_header ){
		void * f = _BSWHP_get_function("bambu_network_track_header");
		func_bambu_network_track_header = ((int(*)(void *agent, std::string header))f);
	}

	return func_bambu_network_track_header(agent, header);
}


int (*func_bambu_network_track_update_property)(void *agent, std::string name, std::string value, std::string type) = NULL;

extern "C" int bambu_network_track_update_property(void *agent, std::string name, std::string value, std::string type){
	_BSWHPLOG_VERBOSE("bambu_network_track_update_property");

	if ( !func_bambu_network_track_update_property ){
		void * f = _BSWHP_get_function("bambu_network_track_update_property");
		func_bambu_network_track_update_property = ((int(*)(void *agent, std::string name, std::string value, std::string type))f);
	}

	return func_bambu_network_track_update_property(agent, name, value, type);
}


int (*func_bambu_network_track_get_property)(void *agent, std::string name, std::string& value, std::string type) = NULL;

extern "C" int bambu_network_track_get_property(void *agent, std::string name, std::string& value, std::string type){
	_BSWHPLOG_VERBOSE("bambu_network_track_get_property");

	if ( !func_bambu_network_track_get_property ){
		void * f = _BSWHP_get_function("bambu_network_track_get_property");
		func_bambu_network_track_get_property = ((int(*)(void *agent, std::string name, std::string& value, std::string type))f);
	}

	return func_bambu_network_track_get_property(agent, name, value, type);
}


int (*func_bambu_network_put_model_mall_rating)(void *agent, int rating_id, int score, std::string content, std::vector<std::string> images, unsigned int &http_code, std::string &http_error) = NULL;

extern "C" int bambu_network_put_model_mall_rating(void *agent, int rating_id, int score, std::string content, std::vector<std::string> images, unsigned int &http_code, std::string &http_error){
	_BSWHPLOG_VERBOSE("bambu_network_put_model_mall_rating");

	if ( !func_bambu_network_put_model_mall_rating ){
		void * f = _BSWHP_get_function("bambu_network_put_model_mall_rating");
		func_bambu_network_put_model_mall_rating = ((int(*)(void *agent, int rating_id, int score, std::string content, std::vector<std::string> images, unsigned int &http_code, std::string &http_error))f);
	}

	return func_bambu_network_put_model_mall_rating(agent, rating_id, score, content, images, http_code, http_error);
}


int (*func_bambu_network_get_oss_config)(void *agent, std::string &config, std::string country_code, unsigned int &http_code, std::string &http_error) = NULL;

extern "C" int bambu_network_get_oss_config(void *agent, std::string &config, std::string country_code, unsigned int &http_code, std::string &http_error){
	_BSWHPLOG_VERBOSE("bambu_network_get_oss_config");

	if ( !func_bambu_network_get_oss_config ){
		void * f = _BSWHP_get_function("bambu_network_get_oss_config");
		func_bambu_network_get_oss_config = ((int(*)(void *agent, std::string &config, std::string country_code, unsigned int &http_code, std::string &http_error))f);
	}

	return func_bambu_network_get_oss_config(agent, config, country_code, http_code, http_error);
}


int (*func_bambu_network_put_rating_picture_oss)(void *agent, std::string &config, std::string &pic_oss_path, std::string model_id, int profile_id, unsigned int &http_code, std::string &http_error) = NULL;

extern "C" int bambu_network_put_rating_picture_oss(void *agent, std::string &config, std::string &pic_oss_path, std::string model_id, int profile_id, unsigned int &http_code, std::string &http_error){
	_BSWHPLOG_VERBOSE("bambu_network_put_rating_picture_oss");

	if ( !func_bambu_network_put_rating_picture_oss ){
		void * f = _BSWHP_get_function("bambu_network_put_rating_picture_oss");
		func_bambu_network_put_rating_picture_oss = ((int(*)(void *agent, std::string &config, std::string &pic_oss_path, std::string model_id, int profile_id, unsigned int &http_code, std::string &http_error))f);
	}

	return func_bambu_network_put_rating_picture_oss(agent, config, pic_oss_path, model_id, profile_id, http_code, http_error);
}


int (*func_bambu_network_get_model_mall_rating)(void *agent, int job_id, std::string &rating_result, unsigned int &http_code, std::string &http_error) = NULL;

extern "C" int bambu_network_get_model_mall_rating(void *agent, int job_id, std::string &rating_result, unsigned int &http_code, std::string &http_error){
	_BSWHPLOG_VERBOSE("bambu_network_get_model_mall_rating");

	if ( !func_bambu_network_get_model_mall_rating ){
		void * f = _BSWHP_get_function("bambu_network_get_model_mall_rating");
		func_bambu_network_get_model_mall_rating = ((int(*)(void *agent, int job_id, std::string &rating_result, unsigned int &http_code, std::string &http_error))f);
	}

	return func_bambu_network_get_model_mall_rating(agent, job_id, rating_result, http_code, http_error);
}





}

