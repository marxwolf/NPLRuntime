//-----------------------------------------------------------------------------
// Class:	NPLConnection
// Authors:	LiXizhi
// Emails:	LiXizhi@yeah.net
// Company: ParaEngine
// Date:	2009.6.1
// Desc:  
//-----------------------------------------------------------------------------
#include "ParaEngine.h"
#include <boost/bind.hpp>
#include "NPLDispatcher.h"
#include "NPLConnectionManager.h"
#include "NPLMsgIn_parser.h"
#include "NPLMsgOut.h"

#include "NPLConnection.h"

/** @def if not defined, we expect all remote NPL runtime's public file list mapping to be identical 
if defined, different NPL runtime can have different local map and file id map are established dynamically. 
*/
//#define  NPL_DYNAMIC_FILE_ID

/** @def the default maximum output message queue size. this is usually set to very big, such as 1024. 
the send message function will fail (service not available), if the queue is full */
#define DEFAULT_NPL_OUTPUT_QUEUE_SIZE		1024

/** define this to log verbose, such as every incoming and out going messages. */
#define NPL_LOG_VERBOSE

#ifdef NPL_LOG_VERBOSE
/** define this to log verbose error message.*/
#define NPL_LOG_VERBOSE_STREAM_MSG
#endif
/** when the message size is bigger than this number of bytes, we will use m_nCompressionLevel for compression. 
For message smaller than the threshold, we will not compress even m_nCompressionLevel is not 0. 
Default value is 200KB */
#define NPL_AUTO_COMPRESSION_THRESHOLD		2048000

/** whether to enable tcp level keep alive. Keep alive is a system socket feature*/
// #define NPL_INCOMING_KEEPALIVE

NPL::CNPLConnection::CNPLConnection( boost::asio::io_service& io_service, CNPLConnectionManager& manager, CNPLDispatcher& msg_dispatcher )
: m_socket(io_service),	m_connection_manager(manager), m_msg_dispatcher(msg_dispatcher), m_totalBytesIn( 0 ), m_totalBytesOut( 0 ),
m_queueOutput(DEFAULT_NPL_OUTPUT_QUEUE_SIZE), m_state(ConnectionDisconnected), 
m_bDebugConnection(false), m_nCompressionLevel(0), m_nCompressionThreshold(NPL_AUTO_COMPRESSION_THRESHOLD),
m_bKeepAlive(false), m_bEnableIdleTimeout(true), m_nIdleTimeoutMS(0), m_nLastActiveTime(0)
{
	m_queueOutput.SetUseEvent(false);
	// init common fields for input message. 
	m_input_msg.reset();
	m_input_msg.npl_version_major = NPL_VERSION_MAJOR;
	m_input_msg.npl_version_minor = NPL_VERSION_MINOR;
	m_input_msg.m_pConnection = this;
}

NPL::CNPLConnection::~CNPLConnection()
{

}

boost::asio::ip::tcp::socket& NPL::CNPLConnection::socket()
{
	return m_socket;
}

void NPL::CNPLConnection::SetTCPKeepAlive(bool bEnable)
{
	// Implements the SOL_SOCKET/SO_KEEPALIVE socket option. 
	boost::asio::socket_base::keep_alive option(bEnable);
	m_socket.set_option(option);
}


void NPL::CNPLConnection::SetKeepAlive(bool bEnable)
{
	m_bKeepAlive = bEnable;
}

bool NPL::CNPLConnection::IsKeepAliveEnabled()
{
	return m_bKeepAlive;
}

void NPL::CNPLConnection::EnableIdleTimeout(bool bEnable)
{
	m_bEnableIdleTimeout = bEnable;
}

bool NPL::CNPLConnection::IsIdleTimeoutEnabled()
{
	return m_bEnableIdleTimeout;
}

void NPL::CNPLConnection::SetIdleTimeoutPeriod(int nMilliseconds)
{
	m_nIdleTimeoutMS = nMilliseconds;
}

int NPL::CNPLConnection::GetIdleTimeoutPeriod()
{
	return m_nIdleTimeoutMS;
}


void NPL::CNPLConnection::SetNPLRuntimeAddress( NPLRuntimeAddress_ptr runtime_address )
{
	m_address = runtime_address;
}

const string& NPL::CNPLConnection::GetNID() const
{
	if(m_address)
		return m_address->GetNID();
	else
		return ParaEngine::CGlobals::GetString(0);
}

string NPL::CNPLConnection::GetIP()
{
	if(m_address)
		return m_address->GetHost();
	else
		return ParaEngine::CGlobals::GetString(0);
}

string NPL::CNPLConnection::GetPort()
{
	if(m_address)
		return m_address->GetPort();
	else
		return ParaEngine::CGlobals::GetString(0);
}

void NPL::CNPLConnection::SetUseCompression(bool bUseCompression)
{
	if(bUseCompression)
	{
		if(m_nCompressionLevel==0)
			m_nCompressionLevel = -1;
	}
	else
	{
		m_nCompressionLevel = 0;
	}
}

bool NPL::CNPLConnection::IsUseCompression()
{
	return m_nCompressionLevel != 0;
}

void NPL::CNPLConnection::SetCompressionLevel(int nLevel)
{
	m_nCompressionLevel = nLevel;
}

int NPL::CNPLConnection::GetCompressionLevel()
{
	return m_nCompressionLevel;
}

void NPL::CNPLConnection::SetCompressionThreshold(int nThreshold)
{
	m_nCompressionThreshold = nThreshold;
}

int NPL::CNPLConnection::GetCompressionThreshold()
{
	return m_nCompressionThreshold;
}

unsigned int NPL::CNPLConnection::GetLastActiveTime()
{
	return m_nLastActiveTime;
}

void NPL::CNPLConnection::TickSend()
{
	m_nLastActiveTime = GetTickCount();
}

void NPL::CNPLConnection::TickReceive()
{
	m_nLastActiveTime = GetTickCount();
}

int NPL::CNPLConnection::CheckIdleTimeout(unsigned int nCurTime)
{
	if(!m_bEnableIdleTimeout || m_nIdleTimeoutMS == 0 || m_nLastActiveTime == 0)
		return 1;
	if((m_nLastActiveTime+m_nIdleTimeoutMS) < nCurTime)
	{
		// this connection is timed out. 
		if(m_bKeepAlive)
		{
			// TODO: send keep alive message to detect "half-open" connection. 
			return -1;
		}
		else
		{
			// close the timed out connection immediately
#ifdef NPL_LOG_VERBOSE
			if(m_address)
			{
				OUTPUT_LOG1("connection time out (%s/%s) with id (%s). \n", 
					m_address->GetHost().c_str(), m_address->GetPort().c_str(), m_address->GetNID().c_str());
			}
#endif
			return 0;
		}
	}
	return 1;
}


void NPL::CNPLConnection::start()
{
	// update the start time and last send/receive time
	m_nStartTime = GetTickCount();
	m_nLastActiveTime = m_nStartTime;
	
	if(m_address)
	{
		m_state = ConnectionConnected;
		// this is active outgoing connection, we will assume that it is authenticated once connection is established. 
		SetAuthenticated(true);

		// set use compression. 
		bool bUseCompression = m_msg_dispatcher.IsUseCompressionOutgoingConnection();
		SetUseCompression(bUseCompression);
		if(bUseCompression) 
		{
			SetCompressionLevel(m_msg_dispatcher.GetCompressionLevel());
			SetCompressionThreshold(m_msg_dispatcher.GetCompressionThreshold());
		}

		// add active connection to dispatcher;
		m_msg_dispatcher.AddNPLConnection(m_address->GetNID(), shared_from_this());
	}
	else
	{
		m_state = ConnectionConnected;
		// this is incoming connection, NID and NPL address are not known for the moment. 
		// we will assign a temporary NID to this connection so that the NPL runtime state can identify this connection
		// and authenticate if necessary. It is also possible that the NPL runtime state never authenticate and reply 
		// to the anonymous connection via the temporary NID. Temporary NID always begins with "~"

		boost::system::error_code ec;
		boost::asio::ip::tcp::endpoint endpoint = m_socket.remote_endpoint(ec);
		if (ec)
		{
			// An error occurred.
			OUTPUT_LOG1("warning: unable to get remote end point of the incoming NPL connection, because %s\n", ec.message().c_str());
			stop();
			return;
		}

		string host_address = endpoint.address().to_string();
		unsigned int nPort = endpoint.port();
		char tmp[32];
		itoa(nPort, tmp, 10);
		string sPort = tmp;

		// this function is assumed to be called only in the dispatcher thread, so no lock is needed to generate the temp id. 
		static unsigned int s_next_temp_id = 0;
		++s_next_temp_id;
		itoa(s_next_temp_id, tmp, 10);
		string nid = "~";
		nid.append(tmp);

		NPLRuntimeAddress_ptr pAddress(new NPLRuntimeAddress(host_address, sPort, nid));
		SetNPLRuntimeAddress(pAddress);


#ifdef NPL_LOG_VERBOSE
		OUTPUT_LOG1("incoming connection (%s/%s) is established and assigned a temporary id (%s). \n", 
			host_address.c_str(), sPort.c_str(), nid.c_str());
#endif
		// set use compresssion. 
		bool bUseCompression = m_msg_dispatcher.IsUseCompressionIncomingConnection();
		SetUseCompression(bUseCompression);
		if(bUseCompression) 
		{
			SetCompressionLevel(m_msg_dispatcher.GetCompressionLevel());
			SetCompressionThreshold(m_msg_dispatcher.GetCompressionThreshold());
		}

		// add active connection to dispatcher;
		m_msg_dispatcher.AddNPLConnection(m_address->GetNID(), shared_from_this());
	}

	// begin reading
	m_socket.async_read_some(boost::asio::buffer(m_buffer),
		boost::bind(&CNPLConnection::handle_read, shared_from_this(),
		boost::asio::placeholders::error,
		boost::asio::placeholders::bytes_transferred));
}

void NPL::CNPLConnection::stop(bool bRemoveConnection, int nReason)
{
	if(bRemoveConnection)
	{
		m_connection_manager.stop(shared_from_this(), nReason);
	}
	else
	{
		if(nReason == 1)
		{
			SendMessage("connect_overriden", "");
		}
		// Post a call to the stop function so that stop() is safe to call from any thread.
		m_socket.get_io_service().post(boost::bind(&CNPLConnection::handle_stop, shared_from_this()));
	}
}

void NPL::CNPLConnection::handle_stop()
{
	m_state = ConnectionDisconnected;

	boost::system::error_code ec;
	m_socket.close(ec);
	if (ec)
	{
		// An error occurred.
		OUTPUT_LOG1("warning: m_socket.close failed in handle_stop, because %s\n", ec.message().c_str());
	}

	// TODO: give a proper reason for the disconnect, such as user cancel or stream error, etc. 
	handleDisconnect(0);

	// also erase from dispatcher
	m_msg_dispatcher.RemoveNPLConnection(shared_from_this());

#ifdef NPL_LOG_VERBOSE
	if(m_address)
	{
		OUTPUT_LOG1("connection closed (%s/%s) with id (%s). \n", 
			m_address->GetHost().c_str(), m_address->GetPort().c_str(), m_address->GetNID().c_str());
	}
#endif
}

void NPL::CNPLConnection::handle_read( const boost::system::error_code& e, std::size_t bytes_transferred )
{
	if (!e)
	{
		bool bRes = true;
		if( bytes_transferred>0)
		{
			m_totalBytesIn += (int) bytes_transferred;

			if(m_bDebugConnection)
			{
				ParaEngine::CLogger::GetSingleton().Write(m_buffer.data(), bytes_transferred);
			}

			bRes = handleReceivedData(bytes_transferred);
		}
		else
		{
			// this fixed an error, when application is closed, the bytes_transferred==0 message will be received. 
			// this will prevent recursive calls to async_read_some
			if(!m_socket.is_open())
			{
				bRes = false;
			}
		}

		if(bRes)
		{
			// Read some from the server.
			m_socket.async_read_some(boost::asio::buffer(m_buffer),
				boost::bind(&CNPLConnection::handle_read, shared_from_this(),
				boost::asio::placeholders::error, 
				boost::asio::placeholders::bytes_transferred));
		}
		else
		{
			// there is a stream error, we shall close the connection
#ifdef NPL_LOG_VERBOSE_STREAM_MSG
			OUTPUT_LOG("invalid handle_read stream detected. connection will be closed. nid %s \n", GetNID().c_str());
#endif
			m_connection_manager.stop(shared_from_this());
		}
	}
	else if (e == boost::asio::error::operation_aborted)
	{
#ifdef NPL_LOG_VERBOSE_STREAM_MSG
		OUTPUT_LOG("network: handle_read operation aborted. nid %s \n", GetNID().c_str());
#endif
	}
	else
	{
#ifdef NPL_LOG_VERBOSE_STREAM_MSG
		std::string msg = e.message();
		OUTPUT_LOG("network: handle_read stopped, asio msg: %s. Connection will be closed \n", msg.c_str());
#endif
		m_connection_manager.stop(shared_from_this());
	}
}

void NPL::CNPLConnection::handle_write( const boost::system::error_code& e )
{
	if (!e)
	{
		// update the last send/receive time
		TickSend();

		// Initiate graceful connection closure.
		//boost::system::error_code ignored_ec;
		//m_socket.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ignored_ec);
		// send more from the buffer, until the output queue is empty
		NPLMsgOut_ptr* msg=NULL;
		if(m_queueOutput.try_next(&msg) && msg!=NULL)
		{
			boost::asio::async_write(m_socket,
				boost::asio::buffer((*msg)->GetBuffer().c_str(), (*msg)->GetBuffer().size()),
				boost::bind(&CNPLConnection::handle_write, shared_from_this(),
				boost::asio::placeholders::error));
		}
	}
	else if (e != boost::asio::error::operation_aborted)
	{
#ifdef NPL_LOG_VERBOSE_STREAM_MSG
		string err = e.message();
		OUTPUT_LOG("warning: handle_write stopped. connection will be closed \n", err.c_str());
#endif
		// close the connection
		m_connection_manager.stop(shared_from_this());
	}
	else
	{
#ifdef NPL_LOG_VERBOSE_STREAM_MSG
		OUTPUT_LOG("warning: handle_write operation aborted. nid %s \n", GetNID().c_str());
#endif
	}
}

void NPL::CNPLConnection::handle_resolve(const boost::system::error_code& err,boost::asio::ip::tcp::resolver::iterator endpoint_iterator)
{
	if (!err)
	{
		// Attempt a connection to the first endpoint in the list. Each endpoint
		// will be tried until we successfully establish a connection.
		boost::asio::ip::tcp::endpoint endpoint = *endpoint_iterator;
		m_socket.async_connect(endpoint,
			boost::bind(&CNPLConnection::handle_connect, shared_from_this(),
			boost::asio::placeholders::error, ++endpoint_iterator));
	}
	else
	{
		string sError = err.message();
		OUTPUT_LOG( "warning: unable to resolve TCP end point %s \n", sError.c_str());
		// close the connection
		m_connection_manager.stop(shared_from_this());
	}
}

void NPL::CNPLConnection::handle_connect(const boost::system::error_code& err,
										 boost::asio::ip::tcp::resolver::iterator endpoint_iterator)
{
	if (!err)
	{
		m_state = ConnectionConnected;
		handleConnect();

		// connection successfully established, let us start reading from the socket. 
		start();
	}
	else if (endpoint_iterator != boost::asio::ip::tcp::resolver::iterator())
	{
		// That endpoint didn't work, try the next one.
		boost::system::error_code ec;
		m_socket.close(ec);
		if (ec)
		{
			// An error occurred.
			OUTPUT_LOG("warning: m_socket.close failed in handle_connect, because %s\n", ec.message().c_str());
		}
		boost::asio::ip::tcp::endpoint endpoint = *endpoint_iterator;
		m_socket.async_connect(endpoint,
			boost::bind(&CNPLConnection::handle_connect, shared_from_this(),
			boost::asio::placeholders::error, ++endpoint_iterator));
	}
	else
	{
		// unable to connect to any TCP endpoints
		OUTPUT_LOG( "warning: unable to resolve TCP end point %s \n", err.message().c_str());
		// close the connection
		m_connection_manager.stop(shared_from_this());
	}
	m_resolver.reset();
}

void NPL::CNPLConnection::connect()
{
	if( m_state > ConnectionDisconnected )
		return;
	m_state = ConnectionConnecting;

	PE_ASSERT(m_address);

	if(!m_resolver)
		m_resolver.reset(new boost::asio::ip::tcp::resolver(m_socket.get_io_service()));

	boost::asio::ip::tcp::resolver::query query(m_address->GetHost(), m_address->GetPort());

	m_resolver->async_resolve(query,
		boost::bind(&CNPLConnection::handle_resolve, shared_from_this(),
		boost::asio::placeholders::error,
		boost::asio::placeholders::iterator));
}

void NPL::CNPLConnection::GetStatistics( int &totalIn, int &totalOut )
{
	totalIn = m_totalBytesIn;
	totalOut = m_totalBytesOut;
}


NPL::NPLReturnCode NPL::CNPLConnection::SendMessage( const NPLFileName& file_name, const char * code /*= NULL*/, int nLength/*=0*/, int priority/*=0*/ )
{

	NPLMsgOut_ptr msg_out(new NPLMsgOut());
	CNPLMsgOut_gen writer(*msg_out);

#ifdef NPL_DYNAMIC_FILE_ID
	// TODO: we should use this->m_filename_id_map instead of the local dispatcher. 
	int file_id = -1;
#else
	// we expect all remote NPL runtime's public file list mapping to be identical 
	int file_id = m_msg_dispatcher.GetIDByPubFileName(file_name.sRelativePath);
#endif
	if(file_name.sRelativePath == "http")
	{
		// for HTTP request/response message 
		if(nLength<0)
			nLength = strlen(code);
		writer.Append(code, nLength);
	}
	else
	{
		// for NPL message 
		writer.AddFirstLine(file_name, file_id);
		//writer.AddHeaderPair(name,value);
		//writer.AddHeaderPair(name2,value2);

		if(nLength<0)
			nLength = strlen(code);
		writer.AddMsgBody(code, nLength, (nLength<=m_nCompressionThreshold ? 0 : m_nCompressionLevel));
	}
	
	return SendMessage(msg_out);
}

NPL::NPLReturnCode NPL::CNPLConnection::SendMessage( const NPLMessage& msg )
{
	// TODO: 
	return NPL_Error;
}

NPL::NPLReturnCode NPL::CNPLConnection::SendMessage( const char* sCommandName, const char* sCommandData)
{
	NPLMsgOut_ptr msg_out(new NPLMsgOut());
	CNPLMsgOut_gen writer(*msg_out);

	writer.AddFirstLine("npl", sCommandName);
	
	char msg[256];
	snprintf(msg, 255, "msg={data=\"%s\"}", sCommandData);
	int nLength = strlen(msg);
	writer.AddMsgBody(msg, nLength, (nLength<=m_nCompressionThreshold ? 0 : m_nCompressionLevel));

	return SendMessage(msg_out);
}

NPL::NPLReturnCode NPL::CNPLConnection::SendMessage( NPLMsgOut_ptr& msg )
{
	// NOTE: m_state is not protected by a lock. This is ok, since its value is always one of the enumeration type. 
	if( msg->empty() )
		return NPL_OK;

	if(  m_state < ConnectionConnected )
	{
		// NOTE: we will drop all pending messages, since it is not connected. 
		return NPL_ConnectionNotEstablished;
	}

	int nLength = (int)msg->GetBuffer().size();
	NPLMsgOut_ptr * pFront = NULL;
	RingBuffer_Type::BufferStatus bufStatus =  m_queueOutput.try_push_get_front(msg, &pFront);

	if(bufStatus == RingBuffer_Type::BufferFirst)
	{
		if(m_bDebugConnection)
		{
			ParaEngine::CLogger::GetSingleton().Write(msg->GetBuffer().c_str(), nLength);
		}

		PE_ASSERT(pFront!=NULL);
		// LXZ: very tricky code to ensure thread-safety to the buffer.
		// only start the sending task when the buffer is empty, otherwise we will wait for previous send task. 
		// i.e. inside handle_write handler. 
		boost::asio::async_write(m_socket,
			boost::asio::buffer((*pFront)->GetBuffer().c_str(), (*pFront)->GetBuffer().size()),
			boost::bind(&CNPLConnection::handle_write, shared_from_this(),
			boost::asio::placeholders::error));
	}
	else if(bufStatus == RingBuffer_Type::BufferOverFlow)
	{
#ifdef NPL_LOG_VERBOSE_STREAM_MSG
		OUTPUT_LOG("NPL SendMessage error because the output msg queue is full. The connection nid is %s \n", GetNID().c_str());
#endif
		// too many messages to send.
		return NPL_QueueIsFull;
	}
	m_totalBytesOut += nLength;
	return NPL_OK;
}

void NPL::CNPLConnection::handleConnect()
{
	m_msg_dispatcher.PostNetworkEvent(NPL_ConnectionEstablished, GetNID().c_str());
}

void NPL::CNPLConnection::handleDisconnect( int reason )
{
	m_msg_dispatcher.PostNetworkEvent(NPL_ConnectionDisconnected, GetNID().c_str());
}

bool NPL::CNPLConnection::handleReceivedData( int bytes_transferred )
{
	boost::tribool result = true;
	Buffer_Type::iterator curIt = m_buffer.begin(); 
	Buffer_Type::iterator curEnd = m_buffer.begin() + bytes_transferred; 

	while (curIt!=curEnd)
	{
		boost::tie(result, curIt) = m_parser.parse(m_input_msg, curIt, curEnd);
		if (result)
		{
			// a complete message is read to m_input_msg.
			handleMessageIn();
		}
		else if (!result)
		{
			m_input_msg.reset();
			m_parser.reset();
			break;
		}
	}

	if (!result)
	{
		// message parsing failed. the message format is not supported. 
		// This is a stream error, we shall close the connection
#ifdef NPL_LOG_VERBOSE_STREAM_MSG
		OUTPUT_LOG("warning: message parsing failed when received data. we will close connection. nid %s \n", GetNID().c_str());
#endif
		m_connection_manager.stop(shared_from_this());
		return false;
	}
	else
	{
		// the message has not been received completely, needs to receive more data from the socket. 
		return true;
	}
}

bool NPL::CNPLConnection::handleMessageIn()
{
	bool bRes = true;

	// update the last send/receive time
	TickReceive();

	// dispatch the message

	if(m_input_msg.npl_version_major==NPL_VERSION_MAJOR /*&& m_input_msg.npl_version_minor==NPL_VERSION_MINOR*/)
	{
		// TODO: some more check on method, uri, headers, etc, before dispatching it.  
		NPLReturnCode nResult = m_msg_dispatcher.DispatchMsg(m_input_msg);

		if( nResult != NPL_OK)
		{
#ifdef NPL_LOG_VERBOSE_STREAM_MSG
			if(nResult == NPL_QueueIsFull)
			{
				OUTPUT_LOG("NPL dispatcher error because NPL_QueueIsFull incoming msg from nid:%s to thread %s\n", GetNID().c_str(), m_input_msg.m_rts_name.c_str());
			}
			else if(nResult == NPL_RuntimeState_NotExist)
			{
				OUTPUT_LOG("NPL dispatcher error because incoming nid:%s NPL_RuntimeState_NotExist\n", GetNID().c_str());
			}
			else
			{
				OUTPUT_LOG("NPL dispatcher error because incoming nid:%s NPLReturnCode %d.\n", GetNID().c_str(), nResult);
			}
#endif
		}

	}
	else
	{
#ifdef NPL_LOG_VERBOSE_STREAM_MSG
		OUTPUT_LOG("NPL protocol version is not supported. nid %s \n", GetNID().c_str());
#endif
		bRes = false;
	}
	return bRes;
}

void NPL::CNPLConnection::SetAuthenticated( bool bAuthenticated )
{
	// no lock is needed. call this function when connection is connected. 
	if(bAuthenticated)
	{
		if(m_state >= ConnectionConnected)
		{
			m_state = ConnectionAuthenticated;
		}
	}
	else
	{
		if(m_state > ConnectionConnected)
		{
			m_state = ConnectionConnected;
		}
	}
}

bool NPL::CNPLConnection::IsAuthenticated() const
{
	return (m_state>=ConnectionAuthenticated);
}

bool NPL::CNPLConnection::IsConnected() const
{
	return (m_state>=ConnectionConnected);
}

bool NPL::CNPLConnection::SetNID( const char* sNID )
{
	// update mapping. 
	if(sNID && m_address)
	{
		m_msg_dispatcher.RenameConnection(shared_from_this(), sNID);
	}
	return true;
}

