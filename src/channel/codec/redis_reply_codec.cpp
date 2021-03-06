 /*
 *Copyright (c) 2013-2013, yinqiwen <yinqiwen@gmail.com>
 *All rights reserved.
 * 
 *Redistribution and use in source and binary forms, with or without
 *modification, are permitted provided that the following conditions are met:
 * 
 *  * Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *  * Neither the name of Redis nor the names of its contributors may be used
 *    to endorse or promote products derived from this software without
 *    specific prior written permission.
 * 
 *THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS 
 *BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF 
 *THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "redis_reply_codec.hpp"
#include "util/buffer_helper.hpp"

#include <limits.h>

using ardb::BufferHelper;
using namespace ardb::codec;
using namespace ardb;

bool RedisReplyEncoder::Encode(Buffer& buf, RedisReply& reply)
{
	switch (reply.type)
	{
		case REDIS_REPLY_NIL:
		{
			buf.Printf("$-1\r\n");
			break;
		}
		case REDIS_REPLY_STRING:
		{
			buf.Printf("$%d\r\n", reply.str.size());
			if (reply.str.size() > 0)
			{
				buf.Printf("%s\r\n", reply.str.c_str());
			}
			else
			{
				buf.Printf("\r\n");
			}
			break;
		}
		case REDIS_REPLY_ERROR:
		{
			buf.Printf("-%s\r\n", reply.str.c_str());
			break;
		}
		case REDIS_REPLY_INTEGER:
		{
			buf.Printf(":%lld\r\n", reply.integer);
			break;
		}
		case REDIS_REPLY_DOUBLE:
		{
			std::string doubleStrValue;
			fast_dtoa(reply.double_value, 9, doubleStrValue);
			buf.Printf("$%d\r\n", doubleStrValue.size());
			buf.Printf("%s\r\n", doubleStrValue.c_str());
			break;
		}
		case REDIS_REPLY_ARRAY:
		{
			buf.Printf("*%d\r\n", reply.elements.size());
			size_t i = 0;
			while (i < reply.elements.size())
			{
				if (!RedisReplyEncoder::Encode(buf, reply.elements[i]))
				{
					return false;
				}
				i++;
			}
			break;
		}
		case REDIS_REPLY_STATUS:
		{
			buf.Printf("+%s\r\n", reply.str.c_str());
			break;
		}
		default:
		{
			ERROR_LOG("Recv unexpected redis reply type:%d", reply.type);
			return false;
		}
	}
	return true;
}

bool RedisReplyEncoder::WriteRequested(ChannelHandlerContext& ctx,
        MessageEvent<RedisReply>& e)
{
	RedisReply* msg = e.GetMessage();
	Buffer buffer(1024);
	if (Encode(buffer, *msg))
	{
		return ctx.GetChannel()->Write(buffer);
	}
	return false;
}

//==================================Decoder==========================================
static const char* kCRLF = "\r\n";
static int decode_reply(Buffer& buffer, RedisReply& msg)
{
	uint32 mark = buffer.GetReadIndex();
	int index = buffer.IndexOf(kCRLF, 2);
	if (-1 == index)
	{
		return 0;
	}
	char type = 0;
	buffer.ReadByte(type);
	switch (type)
	{
		case '$':
		{
			int64 len;
			if (!raw_toint64(buffer.GetRawReadBuffer(),
			        index - buffer.GetReadIndex(), len))
			{
				return -1;
			}
			if (len == -1)
			{
				msg.type = REDIS_REPLY_NIL;
				buffer.SetReadIndex(index + 2);
			}
			else
			{
				buffer.SetReadIndex(index + 2);
				msg.type = REDIS_REPLY_STRING;
				if (buffer.ReadableBytes() < (uint32)(len + 2))
				{
					break;
				}
				msg.str.assign(buffer.GetRawReadBuffer(), len);
				buffer.SkipBytes(len + 2);
			}
			return 1;
		}
		case '+':
		{
			msg.type = REDIS_REPLY_STATUS;
			msg.str.assign(buffer.GetRawReadBuffer(),
			        index - buffer.GetReadIndex());
			buffer.SetReadIndex(index + 2);
			return 1;
		}
		case '-':
		{
			msg.type = REDIS_REPLY_ERROR;
			msg.str.assign(buffer.GetRawReadBuffer(),
			        index - buffer.GetReadIndex());
			buffer.SetReadIndex(index + 2);
			return 1;
		}
		case ':':
		{
			msg.type = REDIS_REPLY_INTEGER;
			int64 i;
			if (!raw_toint64(buffer.GetRawReadBuffer(),
			        index - buffer.GetReadIndex(), i))
			{
				return -1;
			}
			msg.integer = i;
			buffer.SetReadIndex(index + 2);
			return 1;
		}
		case '*':
		{
			int64 len;
			if (!raw_toint64(buffer.GetRawReadBuffer(),
			        index - buffer.GetReadIndex(), len))
			{
				return -1;
			}
			msg.type = REDIS_REPLY_ARRAY;
			buffer.SetReadIndex(index + 2);
			for (int i = 0; i < len; i++)
			{
				RedisReply r;
				int ret = decode_reply(buffer, r);
				if (ret < 0)
				{
					return ret;
				}
				else if (ret == 0)
				{
					break;
				}
				else
				{
					msg.elements.push_back(r);
				}
			}
			return 1;
		}
		default:
		{
			return -1;
		}
	}
	buffer.SetReadIndex(mark);
	return 0;
}
bool RedisReplyDecoder::Decode(ChannelHandlerContext& ctx, Channel* channel,
        Buffer& buffer, RedisReply& msg)
{
	int ret = decode_reply(buffer, msg);
	if (ret < 0)
	{
		//Close conn if recv invalid msg.
		channel->Close();
	}
	return ret > 0;
}
