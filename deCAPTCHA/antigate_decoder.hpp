/*
 * Copyright (C) 2013  微蔡 <microcai@fedoraproject.org>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#pragma once
#include <string>
#include <fstream>
#include <boost/regex.hpp>
#include <boost/random.hpp>
#include <boost/format.hpp>
#include <boost/make_shared.hpp>
#include <boost/asio.hpp>
#include <boost/lexical_cast.hpp>
#include <avhttp.hpp>
#include <avhttp/async_read_body.hpp>

#include <boost/property_tree/ptree.hpp>
namespace pt = boost::property_tree;
#include <boost/property_tree/json_parser.hpp>
namespace js = boost::property_tree::json_parser;

#include <boost/timedcall.hpp>

namespace decaptcha{
namespace decoder{
namespace detail{

template<class Handler>
class antigate_decoder_op : boost::asio::coroutine {

	std::string generate_boundary() const
	{
		boost::rand48 p(time(NULL));
		return  boost::str(boost::format("%06x%06x") % p() %  p()  ).substr(0, 12);
	}
public:
	antigate_decoder_op(boost::asio::io_service & io_service,
			std::string key, std::string host,
			const std::string &buffer, Handler handler)
		: m_io_service(io_service),
		  m_key(key), m_host(host),
		  m_handler(handler),
		  m_stream(boost::make_shared<avhttp::http_stream>(boost::ref(m_io_service))),
		  m_CAPTCHA_ID(boost::make_shared<std::string>()),
		  m_buffers(boost::make_shared<boost::asio::streambuf>()),
		  m_tries(0), stop_tries(false)
	{
		std::string boundary = generate_boundary();
 		std::string content = build_multipart_formdata(buffer, boundary);
 		std::string content_type = boost::str(boost::format("multipart/form-data; boundary=----------------------------%s") % boundary);

		m_stream->request_options(
			avhttp::request_opts()
				(avhttp::http_options::request_method, "POST" )
				(avhttp::http_options::connection, "close" )
				(avhttp::http_options::content_length, boost::lexical_cast<std::string>(content.length()) )
				(avhttp::http_options::content_type, content_type )
				(avhttp::http_options::request_body, content )
		);
		// 处理.
		avhttp::async_read_body(*m_stream, m_host + "in.php", *m_buffers, *this);
	};

	// 这里是 OK|ID_HERE 格式的数据
	void operator()(boost::system::error_code ec, std::size_t bytes_transfered)
	{
		using namespace boost::system::errc;

 		BOOST_ASIO_CORO_REENTER(this)
 		{
			if (!process_upload_result(ec, bytes_transfered))
			{
				m_handler(ec, 0, std::string(""));
				return;
			}

			// 延时 10s
			BOOST_ASIO_CORO_YIELD
					boost::delayedcallsec(m_io_service, 10, boost::asio::detail::bind_handler(*this, ec, 0) );

			do{
				// 延时 5s 然后重试.
				// 这样第一次就延时 15s 了,  正好合适
				BOOST_ASIO_CORO_YIELD
					boost::delayedcallsec(m_io_service, 5, boost::asio::detail::bind_handler(*this, ec, 0) );

				// 获取一下结果
				m_stream = boost::make_shared<avhttp::http_stream>(boost::ref(m_io_service));
				m_buffers = boost::make_shared<boost::asio::streambuf>();

				// http://antigate.com/res.php?key=XXXXX&action=get&id=CAPCHA_ID_HERE
				BOOST_ASIO_CORO_YIELD
					avhttp::async_read_body(*m_stream,
											std::string(boost::str(boost::format("%sres.php?key=%s&action=get&id=%s") % m_host % m_key % *m_CAPTCHA_ID)),
											*m_buffers, *this);

				if (process_result(ec, bytes_transfered))
					return;
			}while (should_try(ec));

			m_handler(make_error_code(operation_canceled), 0, std::string(""));
 		}
	}
private:
	bool process_result(boost::system::error_code & ec, std::size_t bytes_transfered)
	{
 		using namespace boost::system::errc;

		// 检查result
		std::string result;
		result.resize(bytes_transfered);
		m_buffers->sgetn(&result[0], bytes_transfered);
		if ( result == "CAPCHA_NOT_READY")
		{
			return false;
		}

		boost::regex ex("OK\\|([0-9a-zA-Z]*)");
		boost::cmatch what;

 		if (boost::regex_search(result.c_str(), what, ex))
		{
			using namespace boost::asio::detail;
			std::string result_CAPTCHA = what[1];
			m_io_service.post(
					bind_handler(
						m_handler,
						boost::system::error_code(),
						boost::lexical_cast<std::size_t>(*m_CAPTCHA_ID),
						result_CAPTCHA
					)
				);
			return true;
		}
		ec = make_error_code(operation_not_supported);
		stop_tries = true;
		return false;
	}

	bool process_upload_result(boost::system::error_code & ec, std::size_t bytes_transfered)
	{
 		using namespace boost::system::errc;

		// 检查result
		std::string result;
		result.resize(bytes_transfered);
		m_buffers->sgetn(&result[0], bytes_transfered);

		boost::regex ex("OK\\|([0-9]*)");
		boost::cmatch what;

		if (boost::regex_search(result.c_str(), what, ex))
		{
			* m_CAPTCHA_ID = what[1];
			return true;
		}

		ec = make_error_code(operation_not_supported);
		stop_tries = true;
		return false;
	}

	// 神码叫应该继续呢?  就是返回没错误, 也没有超过 1min
	bool should_try(boost::system::error_code ec)
	{
		return stop_tries==false && (m_tries ++ < 5);
	}

private:
	std::string build_multipart_formdata(const std::string &buffer, const std::string & boundary) const
	{
		std::stringstream content_body;


		content_body << "------------------------------" << boundary <<  "\r\n"
					 << "Content-Disposition: form-data; name=\"method\"" <<  "\r\n\r\n"
					 << "post" << "\r\n";
		content_body << "------------------------------" <<  boundary <<  "\r\n"
					 << "Content-Disposition: form-data; name=\"key\"" <<  "\r\n\r\n"
					 << m_key << "\r\n";

		content_body << "------------------------------" <<  boundary <<  "\r\n"
					 << "Content-Disposition: form-data; name=\"regsense\"" <<  "\r\n\r\n"
					 << "0" << "\r\n";

		content_body << "------------------------------" <<  boundary <<  "\r\n"
					 << "Content-Disposition: form-data; name=\"file\"; filename=\"vercode.jpeg\"" << "\r\n"
					 << "Content-Type: image/jpeg" <<  "\r\n\r\n";

		content_body.write(buffer.data(), buffer.length());
			content_body << "\r\n";

		content_body << "------------------------------" <<  boundary << "--" << "\r\n";

		return content_body.str();
	}

private:
	boost::asio::io_service & m_io_service;

	int m_tries;
	bool stop_tries;

	boost::shared_ptr<avhttp::http_stream> m_stream;

	boost::shared_ptr<boost::asio::streambuf> m_buffers;

	boost::shared_ptr<std::string> m_CAPTCHA_ID;

	Handler m_handler;

	const std::string m_key, m_host;
};

}

class antigate_decoder{
public:
	antigate_decoder(boost::asio::io_service & io_service,
		const std::string &key, const std::string & host = "http://antigate.com/")
	  : m_io_service(io_service), m_key(key), m_host(host)
	{
		if ( * m_host.rbegin() != '/' ){
			m_host += "/";
		}
	}

	template <class Handler>
	void operator()(const std::string &buffer, Handler handler)
	{
		detail::antigate_decoder_op<Handler>
				op(m_io_service, m_key, m_host, buffer, handler);
	}

	// 调用这个报告 ID 对应的验证码错误.
	template <class Handler>
	void operator()(std::size_t ID, Handler handler)
	{
		// TODO
	}
private:
	boost::asio::io_service & m_io_service;
	std::string m_key, m_host;
};

}
}
