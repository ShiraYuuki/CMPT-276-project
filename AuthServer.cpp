/*
 Authorization Server code for CMPT 276, Spring 2016.
 */

#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

#include <cpprest/http_listener.h>
#include <cpprest/json.h>

#include <was/common.h>
#include <was/table.h>

#include "TableCache.h"
#include "make_unique.h"

#include "azure_keys.h"

using azure::storage::storage_exception;
using azure::storage::cloud_table;
using azure::storage::cloud_table_client;
using azure::storage::edm_type;
using azure::storage::entity_property;
using azure::storage::table_entity;
using azure::storage::table_operation;
using azure::storage::table_request_options;
using azure::storage::table_result;
using azure::storage::table_shared_access_policy;

using std::cin;
using std::cout;
using std::endl;
using std::getline;
using std::make_pair;
using std::pair;
using std::string;
using std::unordered_map;
using std::vector;

using web::http::http_headers;
using web::http::http_request;
using web::http::methods;
using web::http::status_code;
using web::http::status_codes;
using web::http::uri;

using web::json::value;

using web::http::experimental::listener::http_listener;

using prop_str_vals_t = vector<pair<string,string>>;

constexpr const char* def_url = "http://localhost:34570";

const string auth_table_name {"AuthTable"};
const string auth_table_userid_partition {"Userid"};
const string auth_table_password_prop {"Password"};
const string auth_table_partition_prop {"DataPartition"};
const string auth_table_row_prop {"DataRow"};
const string data_table_name {"DataTable"};

const string get_read_token_op {"GetReadToken"};
const string get_update_token_op {"GetUpdateToken"};
const string get_update_data_op {"GetUpdateData"};

/*
  Cache of opened tables
 */
TableCache table_cache {};

/*
  Convert properties represented in Azure Storage type
  to prop_str_vals_t type.
 */
prop_str_vals_t get_string_properties (const table_entity::properties_type& properties) {
  prop_str_vals_t values {};
  for (const auto v : properties) {
    if (v.second.property_type() == edm_type::string) {
      values.push_back(make_pair(v.first,v.second.string_value()));
    }
    else {
      // Force the value as string in any case
      values.push_back(make_pair(v.first, v.second.str()));
    }
  }
  return values;
}

/*
  Given an HTTP message with a JSON body, return the JSON
  body as an unordered map of strings to strings.

  Note that all types of JSON values are returned as strings.
  Use C++ conversion utilities to convert to numbers or dates
  as necessary.
 */
unordered_map<string,string> get_json_body(http_request message) {  
  unordered_map<string,string> results {};
  const http_headers& headers {message.headers()};
  auto content_type (headers.find("Content-Type"));
  if (content_type == headers.end() ||
      content_type->second != "application/json")
    return results;

  value json{};
  message.extract_json(true)
    .then([&json](value v) -> bool
          {
            json = v;
            return true;
          })
    .wait();

  if (json.is_object()) {
    for (const auto& v : json.as_object()) {
      if (v.second.is_string()) {
        results[v.first] = v.second.as_string();
      }
      else {
        results[v.first] = v.second.serialize();
      }
    }
  }
  return results;
}

/*
  Return a token for 24 hours of access to the specified table,
  for the single entity defind by the partition and row.

  permissions: A bitwise OR ('|')  of table_shared_access_poligy::permission
    constants.

    For read-only:
      table_shared_access_policy::permissions::read
    For read and update: 
      table_shared_access_policy::permissions::read |
      table_shared_access_policy::permissions::update
 */
pair<status_code,string> do_get_token (const cloud_table& data_table,
                   const string& partition,
                   const string& row,
                   uint8_t permissions) {

  utility::datetime exptime {utility::datetime::utc_now() + utility::datetime::from_days(1)};
  try {
    string limited_access_token {
      data_table.get_shared_access_signature(table_shared_access_policy {
                                               exptime,
                                               permissions},
                                             string(), // Unnamed policy
                                             // Start of range (inclusive)
                                             partition,
                                             row,
                                             // End of range (inclusive)
                                             partition,
                                             row)
        // Following token allows read access to entire table
        //table.get_shared_access_signature(table_shared_access_policy {exptime, permissions})
      };
    cout << "Token " << limited_access_token << endl;
    return make_pair(status_codes::OK, limited_access_token);
  }
  catch (const storage_exception& e) {
    cout << "Azure Table Storage error: " << e.what() << endl;
    cout << e.result().extended_error().message() << endl;
    return make_pair(status_codes::InternalError, string{});
  }
}

/*
  Top-level routine for processing all HTTP GET requests.
 */
void handle_get(http_request message) { 
  string path {uri::decode(message.relative_uri().path())};
  cout << endl << "**** AuthServer GET " << path << endl;
  auto paths = uri::split_path(path);
  // Need at least an operation and userid
  if (paths.size() < 2) {
    message.reply(status_codes::BadRequest);
    return;
  }

  // Our extensions =================================================================================================================
  
  unordered_map<string,string> json_body {get_json_body (message)};
  cloud_table table {table_cache.lookup_table(auth_table_name)};

  //getting token
  if(paths[0] == get_read_token_op || paths[0] == get_update_token_op || paths[0] == get_update_data_op){
  	if(json_body.size() == 1){ //only execute if there's only 1 password given
  		string GivenPass;
  		for(const auto v : json_body) { //stores password
  			GivenPass = string(v.second);
    	}
    	//check if there's a given password
      	if(GivenPass == ""){
      		cout << "Invalid password" << endl;
      		message.reply(status_codes::BadRequest);
      		return;
      	}
        for(int i=0; i<GivenPass.size() ;i++){
          if((int)GivenPass[i] > 127 || (int)GivenPass[i] < 0){
            cout << "Invalid password" << endl;
            message.reply(status_codes::BadRequest);
            return;
          }
        }
    	cout << "The Given Password is: " << GivenPass << endl;
    	//here we'll check if such user account exists by searching through the AuthTable
      table_operation retrieve_operation {table_operation::retrieve_entity(auth_table_userid_partition, paths[1])};
      table_result retrieve_result {table.execute(retrieve_operation)};
      cout << "Retrieve User id HTTP code is: " << retrieve_result.http_status_code() << endl;
      if (retrieve_result.http_status_code() == status_codes::NotFound) { //user account not found
        message.reply(status_codes::NotFound);
        return;
      }
      //set up to scan through properties of useraccount
      table_entity entity {retrieve_result.entity()};
      table_entity::properties_type properties {entity.properties()};
      string PartName;
      string RowName;
      bool CorrectPass {false};
      for(const auto v: properties){ //scan through properties
        if(v.first == auth_table_password_prop && string(v.second.string_value()) == GivenPass){
          cout << "Monkey entered correct password!" << endl;
          CorrectPass= true;
        }
        else if(v.first == auth_table_row_prop){
          RowName = string(v.second.string_value());
          cout << "Row is: " << RowName << endl;
        }
        else if(v.first == auth_table_partition_prop){
          PartName = string(v.second.string_value());
          cout << "Partition is: " << PartName << endl;
        }
      }
      if(PartName == "" || RowName == ""){ //check if account has parrtition and row
      	cout << "Bad account" << endl;
      	message.reply(status_codes::BadRequest);
      	return;
      }
      if(CorrectPass){//corect password!
      	cout << "getting token..." << endl;
      	pair<status_code,string> token_obj;
      	//getting requested token, either read only or read and update
      	if(paths[0] == get_read_token_op){ //get read token
      		token_obj = do_get_token(table_cache.lookup_table(data_table_name), PartName, RowName, table_shared_access_policy::permissions::read);
      	}
      	else{ //get update token
      		token_obj = do_get_token(table_cache.lookup_table(data_table_name), PartName, RowName, table_shared_access_policy::permissions::read | table_shared_access_policy::permissions::update);
      	}
        if(token_obj.first == status_codes::OK){
        	cout << "getting token success!" << endl;
        	if(paths[0] == get_update_data_op){
        		//get update token with data!!!
        		message.reply(token_obj.first, value::object(vector<pair<string,value>>{make_pair("token", value::string(token_obj.second)), make_pair(auth_table_partition_prop, value::string(PartName)),  make_pair(auth_table_row_prop, value::string(RowName))}));
        	}
        	else{
        		message.reply(token_obj.first, value::object(vector<pair<string,value>>{make_pair("token", value::string(token_obj.second))}));
        	}
          return;
        }
        else{
        	cout << "getting token failed!" << endl;
        	message.reply(token_obj.first);
          return;
        }
      }
      //bad password!!!
      message.reply(status_codes::NotFound);
      return;
  	}
  }

  //invalid command entered!!!
  message.reply(status_codes::BadRequest);
  return;
  // End of our extensions =================================================================================================================
}

/*
  Top-level routine for processing all HTTP POST requests.
 */
void handle_post(http_request message) {
  string path {uri::decode(message.relative_uri().path())};
  cout << endl << "**** POST " << path << endl;
}

/*
  Top-level routine for processing all HTTP PUT requests.
 */
void handle_put(http_request message) {
  string path {uri::decode(message.relative_uri().path())};
  cout << endl << "**** PUT " << path << endl;
}

/*
  Top-level routine for processing all HTTP DELETE requests.
 */
void handle_delete(http_request message) {
  string path {uri::decode(message.relative_uri().path())};
  cout << endl << "**** DELETE " << path << endl;
}

/*
  Main authentication server routine

  Install handlers for the HTTP requests and open the listener,
  which processes each request asynchronously.

  Note that, unlike BasicServer, AuthServer only
  installs the listeners for GET. Any other HTTP
  method will produce a Method Not Allowed (405)
  response.

  If you want to support other methods, uncomment
  the call below that hooks in a the appropriate 
  listener.
  
  Wait for a carriage return, then shut the server down.
 */
int main (int argc, char const * argv[]) {
  cout << "AuthServer: Parsing connection string" << endl;
  table_cache.init (storage_connection_string);

  cout << "AuthServer: Opening listener" << endl;
  http_listener listener {def_url};
  listener.support(methods::GET, &handle_get);
  //listener.support(methods::POST, &handle_post);
  //listener.support(methods::PUT, &handle_put);
  //listener.support(methods::DEL, &handle_delete);
  listener.open().wait(); // Wait for listener to complete starting

  cout << "Enter carriage return to stop AuthServer." << endl;
  string line;
  getline(std::cin, line);

  // Shut it down
  listener.close().wait();
  cout << "AuthServer closed" << endl;
}
