/*
 *  This file is part of pom-ng.
 *  Copyright (C) 2010-2012 Guy Martin <gmsoft@tuxicoman.be>
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

#ifndef __PTYPE_BOOL_H__
#define __PTYPE_BOOL_H__

#include <pom-ng/ptype.h>

int ptype_bool_mod_register(struct mod_reg *mod);
int ptype_bool_mod_unregister();

int ptype_bool_register(struct ptype_reg *r);
int ptype_bool_alloc(struct ptype *p);
int ptype_bool_cleanup(struct ptype *p);
int ptype_bool_parse(struct ptype *p, char *val);
int ptype_bool_serialize(struct ptype *p, char *val, size_t size);
int ptype_bool_print(struct ptype *pt, char *val, size_t size, char *format);
int ptype_bool_compare(int op, void *val_a, void* val_b);
int ptype_bool_copy(struct ptype *dst, struct ptype *src);
size_t ptype_bool_value_size(struct ptype *pt);

#endif
