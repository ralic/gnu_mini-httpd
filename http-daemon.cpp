/*
 * Copyright (c) 2001-2007 Peter Simons <simons@cryp.to>
 *
 * This software is provided 'as-is', without any express or
 * implied warranty. In no event will the authors be held liable
 * for any damages arising from the use of this software.
 *
 * Copying and distribution of this file, with or without
 * modification, are permitted in any medium without royalty
 * provided the copyright notice and this notice are preserved.
 */

#include "http-daemon.hpp"
#include "io-input-buffer.hpp"
#include "io-output-buffer.hpp"
#include <boost/bind.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <unistd.h>             // close(2)
#include "system-error.hpp"

using namespace std;

// ----- Construction/Destruction ---------------------------------------------

http::daemon::daemon() : _data_fd(-1)
{
  TRACE() << "accepting new connection";
  reset();
}

http::daemon::~daemon()
{
  TRACE() << "closing connection";
  if (_data_fd >= 0) close(_data_fd);
}

/**
 *  \brief (Re-)initialize internals (for use in persistent connections).
 */
void http::daemon::reset()
{
  _inbuf.realloc( 1024u );
  _use_persistent_connection = false;
  _request = Request();
  _request.startup_time = time(0);
  if (_data_fd >= 0) { close(_data_fd); _data_fd = -1; }
  _data_buffer.clear();
  _state = READ_REQUEST_LINE;
}

// ----- State Machine Driver -------------------------------------------------

byte_range http::daemon::get_input_buffer()
{
  return byte_range(_inbuf.end(), _inbuf.buf_end());
}

scatter_vector const & http::daemon::get_output_buffer()
{
  return _outbuf.commit();
}

void http::daemon::append_input(size_t i)
{
  _inbuf.append(i);
  (*this)(_inbuf, _outbuf, i);
}

void http::daemon::drop_output(size_t /* driver always writes full buffer */)
{
  _outbuf.flush();
  if (_state != TERMINATE)
  {
    (*this)(_inbuf, _outbuf, true);
    if (_outbuf.empty())
      _inbuf.flush();
  }
}

/**
 *  \todo protocol_error("Aborting because of excessively long header lines.\r\n");
 */
bool http::daemon::operator() (input_buffer & ibuf, output_buffer & obuf, bool more_input)
{
  TRACE_VAR3(ibuf, obuf, more_input);
  try
  {
    BOOST_ASSERT(_state < TERMINATE);
    _state = (this->*state_handlers[_state])(ibuf, obuf);
    return _state != TERMINATE;
  }
  catch(system_error const & e)       { INFO() << e.what(); }
  catch(std::runtime_error const & e) { INFO() << "run-time error: " << e.what(); }
  catch(std::exception const & e)     { INFO() << "program error: " << e.what(); }
  catch(...)                          { INFO() << "unspecified error condition"; }
  return false;
}

// ----- HTTP state machine ---------------------------------------------------

/**
 *  State handler configuration.
 */
http::daemon::state_fun_t const http::daemon::state_handlers[TERMINATE] =
  { &http::daemon::get_request_line
  , &http::daemon::get_request_header
  , &http::daemon::get_request_body
  , &http::daemon::setup_response
  , &http::daemon::write_response
  };

http::daemon::state_t http::daemon::get_request_line(input_buffer & ibuf, output_buffer & obuf)
{
  TRACE_VAR2(ibuf, obuf);

  for (char const * p = ibuf.begin(); p != ibuf.end(); ++p)
  {
    if (p[0] == '\r' && p + 1 != ibuf.end() && p[1] == '\n')
    {
      ++p;
      size_t const len( _request.parse_request_line(ibuf.begin(), p + 1) );
      BOOST_ASSERT(!len || len == static_cast<size_t>(p + 1 - ibuf.begin()));
      if (len > 0)
      {
        TRACE() << "Read request line"
          << ": method = "        << _request.method
          << ", http version = "  << _request.major_version << '.' << _request.minor_version
          << ", host = "          << _request.url.host
          << ", port = "          << _request.url.port
          << ", path = '"         << _request.url.path << "'"
          << ", query = '"        << _request.url.query << "'"
          ;
        ibuf.consume(len);
        return get_request_header(ibuf, obuf);
      }
      else
      {
        return protocol_error(obuf, "Invalid HTTP request line.\r\n");
      }
    }
  }
  return READ_REQUEST_LINE;
}

http::daemon::state_t http::daemon::get_request_header(input_buffer & ibuf, output_buffer & obuf)
{
  TRACE_VAR2(ibuf, obuf);

  // An empty line terminates the request header.

  if (ibuf.size() >= 2 && ibuf[0] == '\r' && ibuf[1] == '\n')
  {
    ibuf.consume(2);
    TRACE() << "have complete header: going into READ_REQUEST_BODY state.";
    return get_request_body(ibuf, obuf);
  }

  // If we do have a complete header line in the read buffer,
  // process it. If not, we need more I/O before we can proceed.

  char * p( find_next_line(ibuf.begin(), ibuf.end()) );
  TRACE() << "found line of " << (p - ibuf.begin()) << " bytes";
  if (p != ibuf.end())
  {
    string name, data;
    size_t len( parse_header(name, data, ibuf.begin(), p) );
    TRACE() << "parser consumed " << len << " bytes";
    BOOST_ASSERT(!len || ibuf.begin() + len == p);
    ibuf.reset(p, ibuf.end());
    if (len > 0)
    {
      if (strcasecmp("Host", name.c_str()) == 0)
      {
        if (!_request.parse_host_header(data.data(), data.data() + data.size()))
          return protocol_error(obuf, "Malformed <tt>Host</tt> header.\r\n");
        else
          TRACE() << "Read Host header"
            << ": host = " << _request.host
            << "; port = " << _request.port
            ;
      }
      else if (strcasecmp("If-Modified-Since", name.c_str()) == 0)
      {
        if (!_request.parse_if_modified_since_header(data.data(), data.data() + data.size()))
          INFO() << "Ignoring malformed If-Modified-Since header '" << data << "'";
        else
          TRACE() << "Read If-Modified-Since header: timestamp = " << *_request.if_modified_since;
      }
      else if (strcasecmp("Connection", name.c_str()) == 0)
      {
        TRACE() << "Read Connection header: data = '" << data << "'";
        _request.connection = data;
      }
      else if (strcasecmp("Keep-Alive", name.c_str()) == 0)
      {
        TRACE() << "Read Keep-Alive header: data = '" << data << "'";
        _request.keep_alive = data;
      }
      else if (strcasecmp("User-Agent", name.c_str()) == 0)
      {
        TRACE() << "Read User-Agent header: data = '" << data << "'";
        _request.user_agent = data;
      }
      else if (strcasecmp("Referer", name.c_str()) == 0)
      {
        TRACE() << "Read Referer header: data = '" << data << "'";
        _request.referer = data;
      }
      else
        TRACE() << "Ignoring unknown header: '" << name << "' = '" << data << "'";

      return get_request_header(ibuf, obuf);
    }
    else
      return protocol_error(obuf, "Invalid HTTP request.\r\n");
  }

  return READ_REQUEST_HEADER;
}

/// \todo support request bodies

http::daemon::state_t http::daemon::get_request_body(input_buffer & ibuf, output_buffer & obuf)
{
  TRACE_VAR2(ibuf, obuf);
  return setup_response(ibuf, obuf);
}

/**
 *  \brief to be written
 */
http::daemon::state_t http::daemon::restart(input_buffer & ibuf, output_buffer & obuf)
{
  TRACE() << (_use_persistent_connection ? "keep alive" : "shut down");
  if (_use_persistent_connection)
  {
    reset();
    return get_request_line(ibuf, obuf);
  }
  else
    return TERMINATE;
}

/**
 * This routine will normalize the path of our document root and of the
 * file that our client is trying to access. Then it will check,
 * whether that file is _in_ the document root. As a nice side effect,
 * realpath() will report any kind of error, for instance, if a part of
 * the path gives us "permission denied", does not exist or whatever.
 *
 * I am not happy with this routine, nor am I happy with this whole
 * approach. I'll improve it soon.
 *
 * Greetings to Ralph!
 */

static bool is_path_in_hierarchy(char const * hierarchy, char const * path)
{
  char resolved_hierarchy[PATH_MAX];
  char resolved_path[PATH_MAX];

  if (realpath(hierarchy, resolved_hierarchy) == 0 || realpath(path, resolved_path) == 0)
    return false;
  if (strlen(resolved_hierarchy) > strlen(resolved_path))
    return false;
  if (strncmp(resolved_hierarchy, resolved_path, strlen(resolved_hierarchy)) != 0)
    return false;
  return true;
}

http::daemon::state_t http::daemon::setup_response(input_buffer & ibuf, output_buffer & obuf)
{
  TRACE_VAR2(ibuf, obuf);

  struct stat         file_stat;

  // Now that we have the whole request, we can get to work. Let's
  // start by testing whether we understand the request at all.

  if (_request.method != "GET" && _request.method != "HEAD")
  {
    return protocol_error(obuf, string("<p>This server does not support an HTTP request called <tt>")
                         + escape_html_specials(_request.method) + "</tt>.</p>\r\n");
  }

  // Make sure we have a hostname, and make sure it's in lowercase.

  if (_request.host.empty())
  {
    if (_request.host.empty())
    {
      if (!_config.default_hostname.empty() &&
         (_request.major_version == 0 || (_request.major_version == 1 && _request.minor_version == 0))
         )
        _request.host = _config.default_hostname;
      else
      {
        return protocol_error(obuf, "<p>Your HTTP request did not contain a <tt>Host</tt> header.</p>\r\n");
      }
    }
    else
      _request.host = _request.url.host;
  }
  for (string::iterator i = _request.host.begin(); i != _request.host.end(); ++i)
    *i = tolower(*i);

  // Make sure we have a port number.

  if (!_request.port && _request.url.port)
      _request.port = _request.url.port;

  // Construct the actual file name associated with the hostname and
  // URL, then check whether we can send that file.

  string const  document_root( _config.document_root + "/" + _request.host );
  string        filename( document_root + urldecode(_request.url.path) );

  if (!is_path_in_hierarchy(document_root.c_str(), filename.c_str()))
  {
    if (errno != ENOENT)
    {
      INFO() << "peer requested URL "
        << "'http://" << _request.host
        << ':' << (_request.port ? _request.port : 80u)
        << _request.url.path
        << "' ('" << filename << "'), which fails the hierarchy check"
        ;
    }
    return file_not_found(obuf, filename);
  }

 stat_again:
  if (stat(filename.c_str(), &file_stat) == -1)
  {
    if (errno != ENOENT)
    {
      INFO() << "peer requested URL "
        << "'http://" << _request.host
        << ':' << (!_request.port ? 80 : _request.port)
        << _request.url.path
        << "' ('" << filename << "'), which fails stat(2): "
        << ::strerror(errno)
        ;
    }
    return file_not_found(obuf, filename);
  }

  if (S_ISDIR(file_stat.st_mode))
  {
    if (*_request.url.path.rbegin() == '/')
    {
      filename += _config.default_page;
      goto stat_again;
    }
    else
    {
      return moved_permanently(obuf, _request.url.path + "/");
    }
  }

  // Decide whether to use a persistent connection.

  _use_persistent_connection = _request.supports_persistent_connection();

  // Check whether the If-Modified-Since header applies.

  if (_request.if_modified_since)
  {
    if (file_stat.st_mtime <= *_request.if_modified_since)
    {
      TRACE() << "Requested file ('" << filename << "')"
              << " has mtime '" << file_stat.st_mtime
              << " and if-modified-since was " << *_request.if_modified_since
              << ": Not modified.";
      not_modified(obuf);
      return restart(ibuf, obuf);
    }
    else
      TRACE() << "Requested file ('" << filename << "')"
               << " has mtime '" << file_stat.st_mtime
               << " and if-modified-since was " << *_request.if_modified_since
               << ": Modified."
        ;
  }

  // Now answer the request, which may be either HEAD or GET.

  ostringstream buf;
  buf << "HTTP/1.1 200 OK\r\n";
  if (!_config.server_string.empty())
    buf << "Server: " << _config.server_string << "\r\n";
  buf << "Date: " << to_rfcdate(time(0)) << "\r\n"
      << "Content-Type: " << _config.get_content_type(filename.c_str()) << "\r\n"
      << "Content-Length: " << file_stat.st_size << "\r\n"
      << "Last-Modified: " << to_rfcdate(file_stat.st_mtime) << "\r\n";
  if (_use_persistent_connection)  buf << "Connection: keep-alive\r\n";
  else                             buf << "Connection: close\r\n";
  buf << "\r\n";
  _request.status_code = 200;
  _request.object_size = file_stat.st_size;
  string const & iob( buf.str() );
  obuf.push_back(iob.begin(), iob.end());

  // Setup access to the payload and write it.

  BOOST_ASSERT(_data_fd < 0);
  _data_fd = open(filename.c_str(), O_RDONLY);
  if (_data_fd < 0) throw system_error((string("open(\"") + filename + "\") failed"));
  _data_buffer.resize( min(max(1024u, _config.io_block_size), static_cast<size_t>(file_stat.st_size)) );
  log_access();
  return write_response(ibuf, obuf);
}

http::daemon::state_t http::daemon::write_response(input_buffer & ibuf, output_buffer & obuf)
{
  TRACE_VAR2(ibuf, obuf);
  BOOST_ASSERT(_data_fd > 0);
  ssize_t const rc( ::read(_data_fd, &_data_buffer[0], _data_buffer.size()) );
  if (rc < 0)   throw system_error("read(2) failed");
  if (rc == 0)  return restart(ibuf, obuf);
  obuf.append(&_data_buffer[0], static_cast<size_t>(rc));
  return WRITE_RESPONSE;
}

// ----- Logging --------------------------------------------------------------

static string & escape_quotes(string & input)
{
  boost::algorithm::replace_all(input, "\"", "\\\"");
  return input;
}

static string to_logdate(time_t t)
{
  char buffer[64];
  size_t len = strftime(buffer, sizeof(buffer), "%d/%b/%Y:%H:%M:%S %z", localtime(&t));
  if (len == 0 || len >= sizeof(buffer))
    throw length_error("strftime() failed because the internal buffer is too small");
  return buffer;
}

/// \todo Don't re-open the file every time an access is written. A message
///       queue is called for.

void http::daemon::log_access()
{
  BOOST_ASSERT(_request.status_code);

  if (_config.logfile_root.empty()) return;

  string logfile( _config.logfile_root + "/" );
  logfile += _request.host.empty() ? "no-hostname" : _request.host + "-access";

  ofstream os(logfile.c_str(), ios_base::out | ios_base::app);
  if (!os.is_open())
    throw system_error((string("Can't open logfile '") + logfile + "'"));

  // "%s - - [%s] \"%s %s HTTP/%u.%u\" %u %s \"%s\" \"%s\"\n"
  os   << "peer-name-here" << " - - [" << to_logdate(_request.startup_time) << "]"
       << " \"" << _request.method
       << " "  << escape_quotes(_request.url.path)
       << " HTTP/" << _request.major_version
       << "." << _request.minor_version << "\""
       << " " << *_request.status_code
       << " ";
  if (_request.object_size)
    os << *_request.object_size;
  else
    os << "-";
  os   << " \"" << escape_quotes(_request.referer) << "\""
       << " \"" << escape_quotes(_request.user_agent) << "\""
       << endl;
}

// ----- Standard Responses ---------------------------------------------------

http::daemon::state_t http::daemon::protocol_error(output_buffer & obuf, std::string const & message)
{
  INFO() << "protocol error: " << message;

  ostringstream buf;
  buf << "HTTP/1.1 400 Bad Request\r\n";
  if (!_config.server_string.empty())
    buf << "Server: " << _config.server_string << "\r\n";
  buf << "Date: " << to_rfcdate(time(0)) << "\r\n"
      << "Content-Type: text/html\r\n";
  if (!_request.connection.empty())
    buf << "Connection: close\r\n";
  buf << "\r\n"
      << "<html>\r\n"
      << "<head>\r\n"
      << "  <title>Bad HTTP Request</title>\r\n"
      << "</head>\r\n"
      << "<body>\r\n"
      << "<h1>Bad HTTP Request</h1>\r\n"
      << "<p>The HTTP request received by this server was incorrect:</p>\r\n"
      << "<blockquote>\r\n"
      << message
      << "</blockquote>\r\n"
      << "</body>\r\n"
      << "</html>\r\n";
  _request.status_code = 400;
  _request.object_size = 0;
  _use_persistent_connection = false;
  string const & iob( buf.str() );
  obuf.push_back(iob.begin(), iob.end());
  return TERMINATE;
}

http::daemon::state_t http::daemon::file_not_found(output_buffer & obuf, std::string const & path)
{
  INFO() << "not found: URL '" << _request.url.path << "', path '" << path << "'";

  ostringstream buf;
  buf << "HTTP/1.1 404 Not Found\r\n";
  if (!_config.server_string.empty())
    buf << "Server: " << _config.server_string << "\r\n";
  buf << "Date: " << to_rfcdate(time(0)) << "\r\n"
      << "Content-Type: text/html\r\n";
  if (!_request.connection.empty())
    buf << "Connection: close\r\n";
  buf << "\r\n"
      << "<html>\r\n"
      << "<head>\r\n"
      << "  <title>Page Not Found</title>\r\n"
      << "</head>\r\n"
      << "<body>\r\n"
      << "<h1>Page Not Found</h1>\r\n"
      << "<p>The requested page <tt>"
      << escape_html_specials(_request.url.path)
      << "</tt> does not exist on this server.</p>\r\n"
      << "</body>\r\n"
      << "</html>\r\n";
  _request.status_code = 404;
  _request.object_size = 0;
  _use_persistent_connection = false;
  string const & iob( buf.str() );
  obuf.push_back(iob.begin(), iob.end());
  return TERMINATE;
}

http::daemon::state_t http::daemon::moved_permanently(output_buffer & obuf, std::string const & path)
{
  INFO() << "Requested page " << _request.url.path << " has moved to '" << path << "'";

  ostringstream buf;
  buf << "HTTP/1.1 301 Moved Permanently\r\n";
  if (!_config.server_string.empty())
    buf << "Server: " << _config.server_string << "\r\n";
  buf << "Date: " << to_rfcdate(time(0)) << "\r\n"
      << "Content-Type: text/html\r\n"
      << "Location: http://" << _request.host;
  if (_request.port && _request.port != 80)
    buf << ":" << _request.port;
  buf << path << "\r\n";
  if (!_request.connection.empty())
    buf << "Connection: close\r\n";
  buf << "\r\n"
      << "<html>\r\n"
      << "<head>\r\n"
      << "  <title>Page has moved permanently/title>\r\n"
      << "</head>\r\n"
      << "<body>\r\n"
      << "<h1>Document Has Moved</h1>\r\n"
      << "<p>The document has moved <a href=\"http://" << _request.host;
  if (_request.port && _request.port != 80)
    buf << ":" << _request.port;
  buf << path << "\">here</a>.\r\n"
      << "</body>\r\n"
      << "</html>\r\n";
  _request.status_code = 301;
  _request.object_size = 0;
  _use_persistent_connection = false;
  string const & iob( buf.str() );
  obuf.push_back(iob.begin(), iob.end());
  return TERMINATE;
}

void http::daemon::not_modified(output_buffer & obuf)
{
  TRACE() << "requested page not modified";

  ostringstream buf;
  buf << "HTTP/1.1 304 Not Modified\r\n";
  if (!_config.server_string.empty())
    buf << "Server: " << _config.server_string << "\r\n";
  buf << "Date: " << to_rfcdate(time(0)) << "\r\n";
  if (_use_persistent_connection)  buf << "Connection: keep-alive\r\n";
  else                             buf << "Connection: close\r\n";
  buf << "\r\n";
  _request.status_code = 304;
  string const & iob( buf.str() );
  obuf.push_back(iob.begin(), iob.end());
}


// ----- Configuration --------------------------------------------------------

//
// The original getopt()-based parser was:
//
//   char const * optstring = "hdp:r:l:s:u:g:DH:";
//   const option longopts[] =
//     {
//       { "help",               no_argument,       0, 'h' },
//       { "version",            no_argument,       0, 'v' },
//       { "debug",              no_argument,       0, 'd' },
//       { "port",               required_argument, 0, 'p' },
//       { "change-root",        required_argument, 0, 'r' },
//       { "logfile-directory",  required_argument, 0, 'l' },
//       { "server-string",      required_argument, 0, 's' },
//       { "uid",                required_argument, 0, 'u' },
//       { "gid",                required_argument, 0, 'g' },
//       { "no-detach",          required_argument, 0, 'D' },
//       { "default-hostname",   required_argument, 0, 'H' },
//       { "document-root",      required_argument, 0, 'y' },
//       { "default-page",       required_argument, 0, 'z' },
//       { 0, 0, 0, 0 }          // mark end of array
//     };

http::configuration::configuration()
  : default_content_type("application/octet-stream")
  , io_block_size( 1024u )
{
  // Initialize the content type lookup map.

  content_types["ai"]      = "application/postscript";
  content_types["aif"]     = "audio/x-aiff";
  content_types["aifc"]    = "audio/x-aiff";
  content_types["aiff"]    = "audio/x-aiff";
  content_types["asc"]     = "text/plain";
  content_types["au"]      = "audio/basic";
  content_types["avi"]     = "video/x-msvideo";
  content_types["bcpio"]   = "application/x-bcpio";
  content_types["bmp"]     = "image/bmp";
  content_types["cdf"]     = "application/x-netcdf";
  content_types["cpio"]    = "application/x-cpio";
  content_types["cpt"]     = "application/mac-compactpro";
  content_types["csh"]     = "application/x-csh";
  content_types["css"]     = "text/css";
  content_types["dcr"]     = "application/x-director";
  content_types["dir"]     = "application/x-director";
  content_types["doc"]     = "application/msword";
  content_types["dvi"]     = "application/x-dvi";
  content_types["dxr"]     = "application/x-director";
  content_types["eps"]     = "application/postscript";
  content_types["etx"]     = "text/x-setext";
  content_types["gif"]     = "image/gif";
  content_types["gtar"]    = "application/x-gtar";
  content_types["hdf"]     = "application/x-hdf";
  content_types["hqx"]     = "application/mac-binhex40";
  content_types["htm"]     = "text/html";
  content_types["html"]    = "text/html";
  content_types["ice"]     = "x-conference/x-cooltalk";
  content_types["ief"]     = "image/ief";
  content_types["iges"]    = "model/iges";
  content_types["igs"]     = "model/iges";
  content_types["jpe"]     = "image/jpeg";
  content_types["jpeg"]    = "image/jpeg";
  content_types["jpg"]     = "image/jpeg";
  content_types["js"]      = "application/x-javascript";
  content_types["kar"]     = "audio/midi";
  content_types["latex"]   = "application/x-latex";
  content_types["man"]     = "application/x-troff-man";
  content_types["me"]      = "application/x-troff-me";
  content_types["mesh"]    = "model/mesh";
  content_types["mid"]     = "audio/midi";
  content_types["midi"]    = "audio/midi";
  content_types["mov"]     = "video/quicktime";
  content_types["movie"]   = "video/x-sgi-movie";
  content_types["mp2"]     = "audio/mpeg";
  content_types["mp3"]     = "audio/mpeg";
  content_types["mpe"]     = "video/mpeg";
  content_types["mpeg"]    = "video/mpeg";
  content_types["mpg"]     = "video/mpeg";
  content_types["mpga"]    = "audio/mpeg";
  content_types["ms"]      = "application/x-troff-ms";
  content_types["msh"]     = "model/mesh";
  content_types["nc"]      = "application/x-netcdf";
  content_types["oda"]     = "application/oda";
  content_types["pbm"]     = "image/x-portable-bitmap";
  content_types["pdb"]     = "chemical/x-pdb";
  content_types["pdf"]     = "application/pdf";
  content_types["pgm"]     = "image/x-portable-graymap";
  content_types["pgn"]     = "application/x-chess-pgn";
  content_types["png"]     = "image/png";
  content_types["pnm"]     = "image/x-portable-anymap";
  content_types["ppm"]     = "image/x-portable-pixmap";
  content_types["ppt"]     = "application/vnd.ms-powerpoint";
  content_types["ps"]      = "application/postscript";
  content_types["qt"]      = "video/quicktime";
  content_types["ra"]      = "audio/x-realaudio";
  content_types["ram"]     = "audio/x-pn-realaudio";
  content_types["ras"]     = "image/x-cmu-raster";
  content_types["rgb"]     = "image/x-rgb";
  content_types["rm"]      = "audio/x-pn-realaudio";
  content_types["roff"]    = "application/x-troff";
  content_types["rpm"]     = "audio/x-pn-realaudio-plugin";
  content_types["rtf"]     = "application/rtf";
  content_types["rtf"]     = "text/rtf";
  content_types["rtx"]     = "text/richtext";
  content_types["sgm"]     = "text/sgml";
  content_types["sgml"]    = "text/sgml";
  content_types["sh"]      = "application/x-sh";
  content_types["shar"]    = "application/x-shar";
  content_types["silo"]    = "model/mesh";
  content_types["sit"]     = "application/x-stuffit";
  content_types["skd"]     = "application/x-koan";
  content_types["skm"]     = "application/x-koan";
  content_types["skp"]     = "application/x-koan";
  content_types["skt"]     = "application/x-koan";
  content_types["snd"]     = "audio/basic";
  content_types["spl"]     = "application/x-futuresplash";
  content_types["src"]     = "application/x-wais-source";
  content_types["sv4cpio"] = "application/x-sv4cpio";
  content_types["sv4crc"]  = "application/x-sv4crc";
  content_types["swf"]     = "application/x-shockwave-flash";
  content_types["t"]       = "application/x-troff";
  content_types["tar"]     = "application/x-tar";
  content_types["tcl"]     = "application/x-tcl";
  content_types["tex"]     = "application/x-tex";
  content_types["texi"]    = "application/x-texinfo";
  content_types["texinfo"] = "application/x-texinfo";
  content_types["tif"]     = "image/tiff";
  content_types["tiff"]    = "image/tiff";
  content_types["tr"]      = "application/x-troff";
  content_types["tsv"]     = "text/tab-separated-values";
  content_types["txt"]     = "text/plain";
  content_types["ustar"]   = "application/x-ustar";
  content_types["vcd"]     = "application/x-cdlink";
  content_types["vrml"]    = "model/vrml";
  content_types["wav"]     = "audio/x-wav";
  content_types["wrl"]     = "model/vrml";
  content_types["xbm"]     = "image/x-xbitmap";
  content_types["xls"]     = "application/vnd.ms-excel";
  content_types["xml"]     = "text/xml";
  content_types["xpm"]     = "image/x-xpixmap";
  content_types["xwd"]     = "image/x-xwindowdump";
  content_types["xyz"]     = "chemical/x-pdb";
  content_types["zip"]     = "application/zip";
  content_types["hpp"]     = "text/plain";
  content_types["cpp"]     = "text/plain";
}

char const * http::configuration::get_content_type(char const * filename) const
{
  char const * last_dot;
  char const * current;
  for (current = filename, last_dot = 0; *current != '\0'; ++current)
    if (*current == '.')
      last_dot = current;
  if (last_dot == 0)
    return default_content_type.c_str();
  else
    ++last_dot;
  map_t::const_iterator i = content_types.find(last_dot);
  if (i != content_types.end())
    return i->second;
  else
    return default_content_type.c_str();
}

http::configuration http::daemon::_config;
