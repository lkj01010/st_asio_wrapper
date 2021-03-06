#ifndef PACKER_UNPACKER_H_
#define PACKER_UNPACKER_H_

#include "../include/st_asio_wrapper_packer.h"
#include "../include/st_asio_wrapper_unpacker.h"
using namespace st_asio_wrapper;

#ifdef _MSC_VER
#define __off64_t __int64
#define fseeko64 _fseeki64
#define ftello64 _ftelli64
#endif

#define ORDER_LEN	sizeof(char)
#define OFFSET_LEN	sizeof(__off64_t)
#define DATA_LEN	sizeof(__off64_t)

#if defined(_WIN64) || 64 == __WORDSIZE
#define __off64_t_format "%ld"
#else
#define __off64_t_format "%lld"
#endif

/*
protocol:
head(1 byte) + body

if head equal:
0: body is a filename
	request the file length, client->server->client
	return: same head + file length(8 bytes)
1: body is file offset(8 bytes) + data length(8 bytes)
	request the file content, client->server->client
	return: file content(no-protocol), repeat until all data requested by client been sent(only need to request one time)
2: body is talk content
	talk, client->server or server->client
	return: n/a
*/

//demonstrate how to change packer and unpacker at runtime.
class i_file_buffer
{
protected:
	virtual ~i_file_buffer() {}

public:
	virtual bool empty() const = 0;
	virtual size_t size() const = 0;
	virtual const char* data() const = 0;
};

class file_buffer
{
public:
	file_buffer() {}
	file_buffer(const boost::shared_ptr<i_file_buffer>& _buffer) : buffer(_buffer) {}

	void raw_buffer(const boost::shared_ptr<i_file_buffer>& _buffer) {buffer = _buffer;}
	boost::shared_ptr<i_file_buffer> raw_buffer() {return buffer;}

	//the following five functions are needed by st_asio_wrapper
	//for other functions, depends on the implementation of your packer and unpacker
	bool empty() const {return !buffer || buffer->empty();}
	size_t size() const {return buffer ? buffer->size() : 0;}
	const char* data() const {return buffer ? buffer->data() : NULL;}
	void swap(file_buffer& other) {buffer.swap(other.buffer);}

protected:
	boost::shared_ptr<i_file_buffer> buffer;
};

class command : public std::string, public i_file_buffer
{
public:
	virtual bool empty() const {return std::string::empty();}
	virtual size_t size() const {return std::string::size();}
	virtual const char* data() const {return std::string::data();}
};

class command_packer : public i_packer<file_buffer>
{
public:
	virtual bool pack_msg(file_buffer& msg, const char* const pstr[], const size_t len[], size_t num, bool native = false)
	{
		packer p;
		std::string str;
		if (p.pack_msg(str, pstr, len, num, native))
		{
			BOOST_AUTO(com, boost::make_shared<command>());
			com->swap(str);
			msg.raw_buffer(com);

			return true;
		}

		return false;
	}
};

class command_unpacker : public i_unpacker<file_buffer>, public unpacker
{
public:
	virtual void reset_state() {unpacker::reset_state();}
	virtual bool parse_msg(size_t bytes_transferred, i_unpacker<file_buffer>::container_type& msg_can)
	{
		unpacker::container_type tmp_can;
		bool unpack_ok = unpacker::parse_msg(bytes_transferred, tmp_can);
		for (BOOST_AUTO(iter, tmp_can.begin()); iter != tmp_can.end(); ++iter)
		{
			BOOST_AUTO(com, boost::make_shared<command>());
			com->swap(*iter);
			msg_can.resize(msg_can.size() + 1);
			msg_can.back().raw_buffer(com);
		}

		//when unpack failed, some successfully parsed msgs may still returned via msg_can(stick package), please note.
		return unpack_ok;
	}

	virtual size_t completion_condition(const boost::system::error_code& ec, size_t bytes_transferred) {return unpacker::completion_condition(ec, bytes_transferred);}
	virtual boost::asio::mutable_buffers_1 prepare_next_recv() {return unpacker::prepare_next_recv();}
};

class data_buffer : public i_file_buffer
{
public:
	data_buffer(FILE* file, __off64_t offset, __off64_t data_len)  : _file(file), _offset(offset), _data_len(data_len)
	{
		assert(NULL != _file);

		buffer = new char[boost::asio::detail::default_max_transfer_size];
		assert(NULL != buffer);

		fseeko64(_file, _offset, SEEK_SET);
		read();
	}

	~data_buffer() {delete[] buffer;}

public:
	virtual bool empty() const {return 0 == buffer_len;}
	virtual size_t size() const {return buffer_len;}
	virtual const char* data() const {return buffer;}

	void read()
	{
		if (0 == _data_len)
			buffer_len = 0;
		else
		{
			buffer_len = _data_len > boost::asio::detail::default_max_transfer_size ? boost::asio::detail::default_max_transfer_size : (size_t) _data_len;
			_data_len -= buffer_len;
			if (buffer_len != fread(buffer, 1, buffer_len, _file))
			{
				printf("fread(" size_t_format ") error!\n", buffer_len);
				buffer_len = 0;
			}
		}
	}

protected:
	FILE* _file;
	char* buffer;
	size_t buffer_len;

	__off64_t _offset, _data_len;
};

class data_unpacker : public i_unpacker<file_buffer>
{
public:
	data_unpacker(FILE* file, __off64_t offset, __off64_t data_len)  : _file(file), _offset(offset), _data_len(data_len)
	{
		assert(NULL != _file);

		buffer = new char[boost::asio::detail::default_max_transfer_size];
		assert(NULL != buffer);

		fseeko64(_file, _offset, SEEK_SET);
	}
	~data_unpacker() {delete[] buffer;}

	__off64_t get_rest_size() const {return _data_len;}

	virtual void reset_state() {_file = NULL; delete[] buffer; buffer = NULL; _offset = _data_len = 0;}
	virtual bool parse_msg(size_t bytes_transferred, i_unpacker<file_buffer>::container_type& msg_can)
	{
		assert(_data_len >= bytes_transferred && bytes_transferred > 0);
		_data_len -= bytes_transferred;

		if (bytes_transferred != fwrite(buffer, 1, bytes_transferred, _file))
		{
			printf("fwrite(" size_t_format ") error!\n", bytes_transferred);
			return false;
		}

		if (0 == _data_len)
			msg_can.resize(msg_can.size() + 1);

		return true;
	}

	virtual size_t completion_condition(const boost::system::error_code& ec, size_t bytes_transferred) {return ec ? 0 : boost::asio::detail::default_max_transfer_size;}
	virtual boost::asio::mutable_buffers_1 prepare_next_recv()
	{
		size_t buffer_len = _data_len > boost::asio::detail::default_max_transfer_size ? boost::asio::detail::default_max_transfer_size : (size_t) _data_len;
		return boost::asio::buffer(buffer, buffer_len);
	}

protected:
	FILE* _file;
	char* buffer;

	__off64_t _offset, _data_len;
};

class base_socket
{
public:
	base_socket() : state(TRANS_IDLE), file(NULL)  {}

protected:
	enum TRANS_STATE {TRANS_IDLE, TRANS_PREPARE, TRANS_BUSY};
	TRANS_STATE state;
	FILE* file;
};

#endif // PACKER_UNPACKER_H_
