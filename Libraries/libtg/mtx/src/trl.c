//
//  trl.c
//  mtx
//
//  Created by Pavel Morozkin on 17.01.14.
//  Copyright (c) 2014 Pavel Morozkin. All rights reserved.
//

#include "../include/trl.h"
#include "../include/api.h"
#include "../include/buf.h"

trl_t trl_init()
{
  trl_t trl;

  return trl;
}

buf_t_ trl_transport(buf_t_ buf)
{
  buf_t_ b = {};
	/*buf_init(&b);	*/

	// intermediate header
	b = api.buf.add_ui32(buf.size);
	b = api.buf.cat(b, buf);

  // add size
  //ui32_t len_ = buf.size + 12;
  //ui8_t * len_ptr = (ui8_t *)&len_;
  //buf_t_ len = api.buf.add(len_ptr, sizeof(buf.size));
  //b = api.buf.cat(b, len);
  // add seq
  //ui32_t seqn = shared_rc_get_seqn();
  //buf_t_ seq = api.buf.add_ui32(seqn);
  //b = api.buf.cat(b, seq);
  // add buf
  //b = api.buf.cat(b, buf);
  // add crc
  //buf_t_ crc = api.crc.crc32(b);
  //b = api.buf.cat(b, crc);

  return b;
}

buf_t_ trl_detransport(buf_t_ a)
{
    buf_t_ empty = {};
    
    if (a.size < 4) {
        api.log.error("trl_detransport: packet too short");
        return empty;
    }
    
    buf_t_ length_buf = api.buf.add(a.data, 4);
    ui32_t declared_length = api.buf.get_ui32(length_buf);
    ui32_t payload_length = a.size - 4;
    
    if (declared_length != payload_length) {
        api.log.error("trl_detransport: len mismatch");
        return empty;
    }
    
    return api.buf.add(a.data + 4, payload_length);
}
