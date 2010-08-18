/*
 *  This file is part of pom-ng.
 *  Copyright (C) 2010 Guy Martin <gmsoft@tuxicoman.be>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */



#ifndef __CORE_H__
#define __CORE_H__

#include <pom-ng/proto.h>
#include <pthread.h>

struct core_thread {
	struct input_client_entry *input;
	pthread_t thread;
	int run; // Indicate if the thread should continue to run or not
	struct packet *pkt;
};

struct core_thread* core_spawn_thread(struct input_client_entry *i);
void *core_process_thread(void *input);
int core_destroy_thread(struct core_thread *t);
int core_process_packet(struct packet *p, struct proto_reg *datalink);

#endif