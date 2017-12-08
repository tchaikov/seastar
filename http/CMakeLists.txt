seastar_ragel_generate (http_response_parser.rl SEASTAR_HTTP_RESPONSE_PARSER_HEADER)
add_custom_target (seastar-http-response-parser SOURCES ${SEASTAR_HTTP_RESPONSE_PARSER_HEADER})

seastar_ragel_generate (request_parser.rl SEASTAR_HTTP_REQUEST_PARSER_HEADER)
add_custom_target (seastar-http-request-parser SOURCES ${SEASTAR_HTTP_REQUEST_PARSER_HEADER})

seastar_add_sources (
  api_docs.hh api_docs.cc
  common.hh common.cc
  exception.hh
  file_handler.hh file_handler.cc
  function_handlers.hh
  handlers.hh
  httpd.hh httpd.cc
  json_path.hh json_path.cc
  matcher.hh matcher.cc
  matchrules.hh
  mime_types.hh mime_types.cc
  reply.hh reply.cc
  request.hh
  routes.hh routes.cc
  transformers.hh transformers.cc)

add_dependencies (seastar
  seastar-http-response-parser
  seastar-http-request-parser)
