/*
 * Copyright (c) 2001 by Peter Simons <simons@ieee.org>.
 * All rights reserved.
 */

#include <unistd.h>
#include "request-handler.hh"
#include "system-error.hh"
using namespace std;

void RequestHandler::register_network_read_handler()
    {
    TRACE();
    if ((network_properties.poll_events & POLLIN) == 0)
	{
	debug("%d: Registering network read handler.", sockfd);
	network_properties.poll_events |= POLLIN;
	mysched.register_handler(sockfd, *this, network_properties);
	}
    else
	debug("%d: Network read handler is registered already.", sockfd);
    }

void RequestHandler::remove_network_read_handler()
    {
    TRACE();
    if ((network_properties.poll_events & POLLIN) != 0)
	{
	debug("%d: Unregistering network read handler.", sockfd);
	network_properties.poll_events &= ~POLLIN;
	mysched.register_handler(sockfd, *this, network_properties);
	}
    else
	debug("%d: Network read handler is not registered already.", sockfd);
    }

void RequestHandler::register_network_write_handler()
    {
    TRACE();
    if ((network_properties.poll_events & POLLOUT) == 0)
	{
	debug("%d: Registering network write handler.", sockfd);
	network_properties.poll_events |= POLLOUT;
	mysched.register_handler(sockfd, *this, network_properties);
	}
    else
	debug("%d: Network write handler is registered already.", sockfd);
    }

void RequestHandler::remove_network_write_handler()
    {
    TRACE();
    if ((network_properties.poll_events & POLLOUT) != 0)
	{
	debug("%d: Unregistering network write handler.", sockfd);
	network_properties.poll_events &= ~POLLOUT;
	mysched.register_handler(sockfd, *this, network_properties);
	}
    else
	debug("%d: Network write handler is not registered already.", sockfd);
    }

void RequestHandler::register_file_read_handler()
    {
    TRACE();
    if ((file_properties.poll_events & POLLIN) == 0)
	{
	debug("%d: Registering file read handler for fd %d.", sockfd, filefd);
	file_properties.poll_events |= POLLIN;
	mysched.register_handler(filefd, *this, file_properties);
	}
    else
	debug("%d: File read handler for fd %d is registered already.", sockfd, filefd);
    }

void RequestHandler::remove_file_read_handler()
    {
    TRACE();
    if ((file_properties.poll_events & POLLIN) != 0)
	{
	debug("%d: Unregistering file read handler for fd %d.", sockfd, filefd);
	file_properties.poll_events &= ~POLLIN;
	mysched.register_handler(filefd, *this, file_properties);
	}
    else
	debug("%d: File read handler for %d is not registered already.", sockfd, filefd);
    }

size_t RequestHandler::myread(int fd, void* buf, size_t size)
    {
    TRACE();
    ssize_t rc = read(fd, buf, size);
    if (rc < 0)
	{
	if (errno == EAGAIN || errno == EINTR)
	    return 0;
	else
	    throw system_error("read() failed with an error");
	}
    return rc;
    }

size_t RequestHandler::mywrite(int fd, const void* buf, size_t size)
    {
    TRACE();
    ssize_t rc = write(fd, buf, size);
    if (rc < 0)
	{
	if (errno == EAGAIN || errno == EINTR)
	    return 0;
	else
	    throw system_error("write() failed with an error");
	}
    return rc;
    }

bool RequestHandler::write_buffer_or_queue()
    {
    TRACE();
    size_t rc = mywrite(sockfd, buffer.data(), buffer.size());
    debug("%d: Wrote %d bytes from buffer to peer.", sockfd, rc);
    if (rc < buffer.size())
	{
	debug("%d: Could not write all %d bytes at once, using buffer for remaining %d bytes.",
	      sockfd, buffer.size(), buffer.size()-rc);
	buffer.erase(0, rc);
	register_network_write_handler();
	return false;
	}
    else
	{
	debug("%d: Wrote the whole buffer contents.", sockfd);
	return true;
	}
    }