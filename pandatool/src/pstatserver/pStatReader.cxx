// Filename: pStatReader.cxx
// Created by:  drose (09Jul00)
// 
////////////////////////////////////////////////////////////////////

#include "pStatReader.h"
#include "pStatServer.h"
#include "pStatMonitor.h"

#include <pStatClientControlMessage.h>
#include <pStatServerControlMessage.h>
#include <pStatFrameData.h>
#include <pStatProperties.h>
#include <datagram.h>
#include <datagramIterator.h>
#include <connectionManager.h>

////////////////////////////////////////////////////////////////////
//     Function: PStatReader::Constructor
//       Access: Public
//  Description: 
////////////////////////////////////////////////////////////////////
PStatReader::
PStatReader(PStatServer *manager, PStatMonitor *monitor) :
  ConnectionReader(manager, monitor->is_thread_safe() ? 1 : 0),
  _manager(manager),
  _monitor(monitor),
  _writer(manager, 0)
{
  _udp_port = 0;
  _client_data = new PStatClientData(this);
  _monitor->set_client_data(_client_data);
}

////////////////////////////////////////////////////////////////////
//     Function: PStatReader::Destructor
//       Access: Public
//  Description: 
////////////////////////////////////////////////////////////////////
PStatReader::
~PStatReader() {
  _manager->release_udp_port(_udp_port);
}

////////////////////////////////////////////////////////////////////
//     Function: PStatReader::close
//       Access: Public
//  Description: This will be called by the PStatClientData in
//               response to its close() call.  It will tell the
//               server to let go of the reader so it can shut down
//               its connection.
////////////////////////////////////////////////////////////////////
void PStatReader::
close() {
  _manager->remove_reader(_tcp_connection, this);
  lost_connection();
}

////////////////////////////////////////////////////////////////////
//     Function: PStatReader::set_tcp_connection
//       Access: Public
//  Description: This is intended to be called only once, immediately
//               after construction, by the PStatListener that created
//               it.  It tells the reader about the newly-established
//               TCP connection to a client.
////////////////////////////////////////////////////////////////////
void PStatReader::
set_tcp_connection(Connection *tcp_connection) {
  _tcp_connection = tcp_connection;
  add_connection(_tcp_connection);

  _udp_port = _manager->get_udp_port();
  _udp_connection = _manager->open_UDP_connection(_udp_port);
  while (_udp_connection.is_null()) {
    // That UDP port was no good.  Try another.
    _udp_port = _manager->get_udp_port();
    _udp_connection = _manager->open_UDP_connection(_udp_port);
  }
  
  add_connection(_udp_connection);

  send_hello();
}

////////////////////////////////////////////////////////////////////
//     Function: PStatReader::lost_connection
//       Access: Public
//  Description: This is called by the PStatServer when it detects
//               that the connection has been lost.  It should clean
//               itself up and shut down nicely.
////////////////////////////////////////////////////////////////////
void PStatReader::
lost_connection() {
  _client_data->_is_alive = false;
  _monitor->lost_connection();
  _client_data.clear();

  _manager->close_connection(_tcp_connection);
  _manager->close_connection(_udp_connection);
  _tcp_connection.clear();
  _udp_connection.clear();
}

////////////////////////////////////////////////////////////////////
//     Function: PStatReader::idle
//       Access: Public
//  Description: Called each frame to do what needs to be done for the
//               monitor's user-defined idle routines.
////////////////////////////////////////////////////////////////////
void PStatReader::
idle() {
  _monitor->idle();
}

////////////////////////////////////////////////////////////////////
//     Function: PStatReader::get_hostname
//       Access: Private
//  Description: Returns the current machine's hostname.
////////////////////////////////////////////////////////////////////
string PStatReader::
get_hostname() {
  if (_hostname.empty()) {
    _hostname = ConnectionManager::get_host_name();
    if (_hostname.empty()) {
      _hostname = "unknown";
    }
  }
  return _hostname;
}

////////////////////////////////////////////////////////////////////
//     Function: PStatReader::send_hello
//       Access: Private
//  Description: Sends the initial greeting message to the client.
////////////////////////////////////////////////////////////////////
void PStatReader::
send_hello() {
  PStatServerControlMessage message;
  message._type = PStatServerControlMessage::T_hello;
  message._server_hostname = get_hostname();
  message._server_progname = _monitor->get_monitor_name();
  message._udp_port = _udp_port;

  Datagram datagram;
  message.encode(datagram);
  _writer.send(datagram, _tcp_connection);
}

////////////////////////////////////////////////////////////////////
//     Function: PStatReader::receive_datagram
//       Access: Private, Virtual
//  Description: Called by the net code whenever a new datagram is
//               detected on a either the TCP or UDP connection.
////////////////////////////////////////////////////////////////////
void PStatReader::
receive_datagram(const NetDatagram &datagram) {
  Connection *connection = datagram.get_connection();

  if (connection == _tcp_connection) {
    PStatClientControlMessage message;
    if (message.decode(datagram, _client_data)) {
      handle_client_control_message(message);

    } else if (message._type == PStatClientControlMessage::T_datagram) {
      handle_client_udp_data(datagram);
      
    } else {
      nout << "Got unexpected message from client.\n";
    }

  } else if (connection == _udp_connection) {
    handle_client_udp_data(datagram);

  } else {
    nout << "Got datagram from unexpected socket.\n"; 
  }
}

////////////////////////////////////////////////////////////////////
//     Function: PStatReader::handle_client_control_message
//       Access: Private
//  Description: Called when a control message has been received by
//               the client over the TCP connection.
////////////////////////////////////////////////////////////////////
void PStatReader::
handle_client_control_message(const PStatClientControlMessage &message) {
  switch (message._type) {
  case PStatClientControlMessage::T_hello:
    {
      _client_data->set_version(message._major_version, message._minor_version);
      int server_major_version = get_current_pstat_major_version();
      int server_minor_version = get_current_pstat_minor_version();

      if (message._major_version != server_major_version ||
          (message._major_version == server_major_version &&
           message._minor_version > server_minor_version)) {
        _monitor->bad_version(message._client_hostname, message._client_progname,
                              message._major_version, message._minor_version,
                              server_major_version, server_minor_version);
        _monitor->close();
      } else {
        _monitor->hello_from(message._client_hostname, message._client_progname);
      }
    }
    break;

  case PStatClientControlMessage::T_define_collectors:
    {
      for (int i = 0; i < (int)message._collectors.size(); i++) {
	_client_data->add_collector(message._collectors[i]);
	_monitor->new_collector(message._collectors[i]->_index);
      }
    }
    break;

  case PStatClientControlMessage::T_define_threads:
    {
      for (int i = 0; i < (int)message._names.size(); i++) {
	int thread_index = message._first_thread_index + i;
	string name = message._names[i];
	_client_data->define_thread(thread_index, name);
	_monitor->new_thread(thread_index);
      }
    }
    break;

  default:
    nout << "Invalid control message received from client.\n";
  }
}

////////////////////////////////////////////////////////////////////
//     Function: PStatReader::handle_client_udp_data
//       Access: Private
//  Description: Called when a UDP datagram has been received by the
//               client.  This should be a single frame's worth of
//               data.
////////////////////////////////////////////////////////////////////
void PStatReader::
handle_client_udp_data(const Datagram &datagram) {
  if (!_monitor->is_client_known()) {
    // If we haven't heard a "hello" from the client yet, we don't
    // know what version data it will be sending us, so we can't
    // decode the data.  Chances are good we can't display it sensibly
    // yet anyway.  Ignore frame data until we get that hello.
    return;
  }

  DatagramIterator source(datagram);

  if (_client_data->is_at_least(2, 1)) {
    // Throw away the zero byte at the beginning.
    int initial_byte = source.get_uint8();
    nassertv(initial_byte == 0);
  }

  int thread_index = source.get_uint16();
  int frame_number = source.get_uint32();
  PStatFrameData *frame_data = new PStatFrameData;
  frame_data->read_datagram(source, _client_data);

  // Check to see if any new collectors have level data.
  int num_levels = frame_data->get_num_levels();
  for (int i = 0; i < num_levels; i++) {
    int collector_index = frame_data->get_level_collector(i);
    if (!_client_data->get_collector_has_level(collector_index)) {
      // This collector is now reporting level data, and it wasn't
      // before.
      _client_data->set_collector_has_level(collector_index, true);
      _monitor->new_collector(collector_index);
    }
  }

  _client_data->record_new_frame(thread_index, frame_number, frame_data);
  _monitor->new_data(thread_index, frame_number);
}
  
