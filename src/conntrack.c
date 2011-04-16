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

#include "proto.h"
#include "conntrack.h"
#include "jhash.h"
#include "common.h"

#include <pthread.h>
#include <pom-ng/timer.h>

#define INITVAL 0x5de97c2d // random value

struct conntrack_tables* conntrack_tables_alloc(size_t tables_size, int has_rev) {

	struct conntrack_tables *ct = malloc(sizeof(struct conntrack_tables));
	if (!ct) {
		pom_oom(sizeof(struct conntrack_tables));
		return NULL;
	}
	memset(ct, 0, sizeof(struct conntrack_tables));

	if (pthread_mutex_init(&ct->lock, NULL)) {
		pomlog(POMLOG_ERR "Could not initialize conntrack tables mutex : %s", pom_strerror(errno));
		free(ct);
		return NULL;
	}

	size_t size = sizeof(struct conntrack_list) * tables_size;
	ct->fwd_table = malloc(size);
	if (!ct->fwd_table) {
		pom_oom(size);
		free(ct);
		return NULL;
	}
	memset(ct->fwd_table, 0, size);


	if (has_rev) {
		ct->rev_table = malloc(size);
		if (!ct->rev_table) {
			free(ct->fwd_table);
			free(ct);
			pom_oom(size);
			return NULL;
		}
		memset(ct->rev_table, 0, size);
	}

	ct->tables_size = tables_size;

	return ct;
}

int conntrack_tables_free(struct conntrack_tables *ct) {

	if (!ct)
		return POM_OK;
	if (ct->fwd_table) {
		int i;
		for (i = 0; i < ct->tables_size; i++) {
			while (ct->fwd_table[i]) {
				struct conntrack_list *tmp = ct->fwd_table[i];
				ct->fwd_table[i] = tmp->next;

				// Free the conntrack entry
				if (tmp->ce) {

					struct conntrack_child_list *child = tmp->ce->children;
					while (child) {
						tmp->ce->children = child->next;
						free(child);
						child = tmp->ce->children;
					}
					
					if (tmp->ce->priv && tmp->ce->proto->info->ct_info.cleanup_handler) {
						if (tmp->ce->proto->info->ct_info.cleanup_handler(tmp->ce) != POM_OK)
							pomlog(POMLOG_WARN "Unable to free the private memory of a conntrack");
					}

					if (tmp->ce->fwd_value)
						ptype_cleanup(tmp->ce->fwd_value);
					if (tmp->ce->rev_value)
						ptype_cleanup(tmp->ce->rev_value);
					free(tmp->ce);
					tmp->ce = NULL;
				} else {
					pomlog(POMLOG_WARN "Forward conntrack list without a conntrack entry");
				}

				if (tmp->rev)
					tmp->rev->ce = NULL;

				free(tmp);
				
			}

		}
		free(ct->fwd_table);

	}

	if (ct->rev_table) {
		int i;
		for (i = 0; i < ct->tables_size; i++) {
			while (ct->rev_table[i]) {
				struct conntrack_list *tmp = ct->rev_table[i];
				ct->rev_table[i] = tmp->next;

				// Free the conntrack entry
				if (tmp->ce) {

					struct conntrack_child_list *child = tmp->ce->children;
					while (child) {
						tmp->ce->children = child->next;
						free(tmp->ce->children);
						child = child->next;
					}

					if (tmp->ce->priv && tmp->ce->proto->info->ct_info.cleanup_handler) {
						if (tmp->ce->proto->info->ct_info.cleanup_handler(tmp->ce) != POM_OK)
							pomlog(POMLOG_WARN "Unable to free the private memory of a conntrack");
					}

					if (tmp->ce->fwd_value)
						ptype_cleanup(tmp->ce->fwd_value);
					if (tmp->ce->rev_value)
						ptype_cleanup(tmp->ce->rev_value);
					free(tmp->ce);
					tmp->ce = NULL;
				}

				free(tmp);
			}
		}
		free(ct->rev_table);
	}

	pthread_mutex_destroy(&ct->lock);

	free(ct);

	return POM_OK;
}

int conntrack_hash(uint32_t *hash, struct ptype *fwd, struct ptype *rev) {

	if (!fwd)
		return POM_ERR;

	size_t size_fwd = ptype_get_value_size(fwd);
	if (size_fwd < 0)
		return POM_ERR;

	if (!rev) {
		// Only fwd direction

		// Try to use the best hash function
		if (size_fwd == sizeof(uint32_t)) { // exactly one word
			*hash = jhash_1word(*((uint32_t*)fwd->value), INITVAL);
		} else if (size_fwd == 2 * sizeof(uint32_t))  { // exactly two words
			*hash = jhash_2words(*((uint32_t*)fwd->value), *((uint32_t*)(fwd->value + sizeof(uint32_t))), INITVAL);
		} else if (size_fwd == 3 * sizeof(uint32_t)) { // exactly 3 words
			*hash = jhash_3words(*((uint32_t*)fwd->value), *((uint32_t*)(fwd->value + sizeof(uint32_t))), *((uint32_t*)(fwd->value + (2 * sizeof(uint32_t)))), INITVAL);
		} else {
			*hash = jhash((char*)fwd->value, size_fwd, INITVAL);
		}
	 } else {
		size_t size_rev = ptype_get_value_size(rev);
		if (size_rev < 0)
			return POM_ERR;

		// Try to use the best hash function
		if (size_fwd == sizeof(uint16_t) && size_rev == sizeof(uint16_t)) { // exactly one word
			*hash = jhash_1word(*((uint16_t*)fwd->value) << 16 | *((uint16_t*)rev->value), INITVAL);
		} else if (size_fwd == sizeof(uint32_t) && size_rev == sizeof(uint32_t)) { // exactly 2 words
			*hash = jhash_2words(*((uint32_t*)fwd->value), *((uint32_t*)rev->value), INITVAL);
		} else {

			uint32_t hash_fwd = jhash((char*)fwd->value, size_fwd, INITVAL);
			*hash = jhash((char*)rev->value, size_rev, hash_fwd);
		}

	 }

	return POM_OK;
}


struct conntrack_entry *conntrack_find(struct conntrack_list *lst, struct ptype *fwd_value, struct ptype *rev_value, struct conntrack_entry *parent) {

	if (!fwd_value)
		return NULL;


	for (; lst; lst = lst->next) {
		struct conntrack_entry *ce = lst->ce;

		// Check the parent conntrack
		if (ce->parent != parent)
			continue;

		// Check the forward value
		if (!ptype_compare_val(PTYPE_OP_EQ, ce->fwd_value, fwd_value))
			continue;
		
		// Check the reverse value if present
		if (ce->rev_value)  {
			if (!rev_value) {
				// Conntrack_entry has a reverse value but none was provided
				continue;
			}

			if (!ptype_compare_val(PTYPE_OP_EQ, ce->rev_value, rev_value))
				continue;

		} else if (rev_value) {
			// Conntrack entry does not have a reverse value but one was provided
			continue;
		}

		return ce;

	}

	// No conntrack entry found
	return NULL;
}

struct conntrack_entry *conntrack_get(struct proto_reg *proto, struct ptype *fwd_value, struct ptype *rev_value, struct conntrack_entry *parent) {

	if (!fwd_value || !proto)
		return NULL;

	uint32_t full_hash_fwd = 0, hash_fwd = 0, full_hash_rev = 0, hash_rev = 0;

	if (conntrack_hash(&full_hash_fwd, fwd_value, rev_value) == POM_ERR) {
		pomlog(POMLOG_ERR "Error while computing forward hash for conntrack");
		return NULL;
	}

	struct conntrack_tables *ct = proto->ct;

	// Lock the tables while browsing for a conntrack
	pom_mutex_lock(&ct->lock);

	if (!ct->fwd_table) {
		pom_mutex_unlock(&ct->lock);
		pomlog(POMLOG_ERR "Cannot get conntrack as the forward table is not allocated");
		return NULL;
	}

	// Try to find the conntrack in the forward table
	hash_fwd = full_hash_fwd % ct->tables_size;

	// Check if we can find this entry in the forward way
	struct conntrack_entry *res = conntrack_find(ct->fwd_table[hash_fwd], fwd_value, rev_value, parent);
	if (res) {
		pom_mutex_unlock(&ct->lock);
		return res;
	}

	// It wasn't found in the forward way, maybe in the reverse direction ?
	if (rev_value) {
		if (conntrack_hash(&full_hash_rev, rev_value, fwd_value) == POM_ERR) {
			pomlog(POMLOG_ERR "Error while computing reverse hash for conntrack");
			pom_mutex_unlock(&ct->lock);
			return NULL;
		}
		hash_rev = full_hash_rev % ct->tables_size;

		res = conntrack_find(ct->rev_table[hash_rev], rev_value, fwd_value, parent);
		if (res) {
			pom_mutex_unlock(&ct->lock);
			return res;
		}

	}

	// It's not found in the reverse direction either, let's create it then

	// Alloc the conntrack entry
	res = malloc(sizeof(struct conntrack_entry));
	if (!res) {
		pom_mutex_unlock(&ct->lock);
		pom_oom(sizeof(struct conntrack_entry));
		return NULL;
	}
	memset(res, 0, sizeof(struct conntrack_entry));

	pthread_mutexattr_t attr;
	if (pthread_mutexattr_init(&attr)) {
		pom_mutex_unlock(&ct->lock);
		free(res);
		pomlog(POMLOG_ERR "Error while initializing conntrack mutex attribute");
		return NULL;
	}

	if (pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE)) {
		pom_mutex_unlock(&ct->lock);
		free(res);
		pomlog(POMLOG_ERR "Error while setting conntrack mutex attribute to recursive");
		return NULL;
	}

	if(pthread_mutex_init(&res->lock, &attr)) {
		pom_mutex_unlock(&ct->lock);
		free(res);
		pomlog(POMLOG_ERR "Error while initializing a conntrack lock : %s", pom_strerror(errno));
		return NULL;
	}

	if (pthread_mutexattr_destroy(&attr)) {
		pomlog(POMLOG_WARN "Error while destroying conntrack mutex attribute");
	}

	if (parent) {

		struct conntrack_child_list *child = malloc(sizeof(struct conntrack_child_list));
		if (!child) {
			pom_mutex_unlock(&ct->lock);
			pom_oom(sizeof(struct conntrack_child_list));
			free(res);
			return NULL;
		}
		memset(child, 0, sizeof(struct conntrack_child_list));

		child->ce = res;

		pom_mutex_lock(&parent->lock);
		child->next = parent->children;
		if (child->next)
			child->next->prev = child;
		parent->children = child;
		pom_mutex_unlock(&parent->lock);
		res->parent = parent;

	}

	res->proto = proto;

	res->fwd_hash = full_hash_fwd;

	res->fwd_value = ptype_alloc_from(fwd_value);
	if (!res->fwd_value)
		goto err;

	// Alloc the forward list
	struct conntrack_list *lst_fwd = malloc(sizeof(struct conntrack_list));
	if (!lst_fwd) {
		ptype_cleanup(res->fwd_value);
		pom_oom(sizeof(struct conntrack_list));
		goto err;
	}
	memset(lst_fwd, 0, sizeof(struct conntrack_list));
	lst_fwd->ce = res;

	// Alloc the reverse list
	if (rev_value) {
		res->rev_hash = full_hash_rev;
		struct conntrack_list *lst_rev = malloc(sizeof(struct conntrack_list));
		if (!lst_rev) {
			ptype_cleanup(res->fwd_value);
			free(lst_fwd);
			pom_oom(sizeof(struct conntrack_list));
			goto err;
		}
		memset(lst_rev, 0, sizeof(struct conntrack_list));
		lst_rev->ce = res;

		res->rev_value = ptype_alloc_from(rev_value);
		if (!res->rev_value) {
			ptype_cleanup(res->fwd_value);
			free(lst_fwd);
			free(lst_rev);
			goto err;
		}

		// Insert the reverse direction in the conntrack table
		lst_rev->next = ct->rev_table[hash_rev];
		if (lst_rev->next)
			lst_rev->next->prev = lst_rev;
		ct->rev_table[hash_rev] = lst_rev;

		lst_fwd->rev = lst_rev;
		lst_rev->rev = lst_fwd;


	}

	// Insert the forward direction in the conntrack table
	lst_fwd->next = ct->fwd_table[hash_fwd];
	if (lst_fwd->next)
		lst_fwd->next->prev = lst_fwd;
	ct->fwd_table[hash_fwd] = lst_fwd;

	// Unlock the tables
	pom_mutex_unlock(&ct->lock);

	return res;

err:
	pthread_mutex_destroy(&res->lock);
	free(res);
	pom_mutex_unlock(&ct->lock);

	return NULL;
}

int conntrack_delayed_cleanup(struct conntrack_entry *ce, unsigned int delay) {

	if (!delay) {
		if (ce->cleanup_timer) {
			timer_dequeue(ce->cleanup_timer);
			timer_cleanup(ce->cleanup_timer);
			ce->cleanup_timer = NULL;
		}
		return POM_OK;
	}

	if (!ce->cleanup_timer) {
		ce->cleanup_timer = timer_alloc(ce, conntrack_cleanup);
		if (!ce->cleanup_timer)
			return POM_ERR;
	} else {
		timer_dequeue(ce->cleanup_timer);
	}

	timer_queue(ce->cleanup_timer, delay);

	return POM_OK;
}


int conntrack_cleanup(void *conntrack) {

	struct conntrack_entry *ce = conntrack;

	pom_mutex_lock(&ce->lock);

	// Cleanup the children
	while (ce->children)
		if (conntrack_cleanup(ce->children->ce) != POM_OK)
			return POM_ERR;

	if (ce->parent) {
		// Remove the child from the parent
		pom_mutex_lock(&ce->parent->lock);
		struct conntrack_child_list *tmp = ce->parent->children;

		for (; tmp; tmp = tmp->next) {
			if (tmp->ce == ce)
				break;
		}

		if (tmp) {
			if (tmp->prev)
				tmp->prev->next = tmp->next;
			else
				ce->parent->children = tmp->next;

			if (tmp->next)
				tmp->next->prev = tmp->prev;

			free(tmp);
		} else {
			pomlog(POMLOG_WARN "Conntrack not found in parent's children list");
		}

		pom_mutex_unlock(&ce->parent->lock);
	}


	if (ce->priv && ce->proto->info->ct_info.cleanup_handler) {
		if (ce->proto->info->ct_info.cleanup_handler(ce) != POM_OK)
			pomlog(POMLOG_WARN "Unable to free the private memory of a conntrack");
	}
	pom_mutex_unlock(&ce->lock);

	struct conntrack_tables *ct = ce->proto->ct;

	// Lock the tables while browsing for a conntrack
	pom_mutex_lock(&ct->lock);

	// Try to find the conntrack in the forward table
	uint32_t hash = ce->fwd_hash % ct->tables_size;

	struct conntrack_list *lst = NULL;

	for (lst = ct->fwd_table[hash]; lst; lst = lst->next)
		if (lst->ce == ce)
			break;
	
	if (!lst) {
		pom_mutex_unlock(&ct->lock);
		pomlog(POMLOG_ERR "Conntrack not found in the list for corresponding hash");
		return POM_ERR;
	}

	if (lst->prev)
		lst->prev->next = lst->next;
	else
		ct->fwd_table[hash] = lst->next;

	if (lst->next)
		lst->next->prev = lst->prev;

	struct conntrack_list *lst_rev = lst->rev;
	
	free(lst);

	if (lst_rev) {
		// Remove it from the reverse table
		hash = ce->rev_hash % ct->tables_size;
		
		if (lst_rev->prev)
			lst_rev->prev->next = lst_rev->next;
		else {
			if (lst_rev != ct->rev_table[hash]) {
				pomlog(POMLOG_ERR "Conntrack list was supposed to be the head of reverse but wasn't !");
				return POM_ERR;
			}
			ct->rev_table[hash] = lst_rev->next;
		}

		if (lst_rev->next)
			lst_rev->next->prev = lst_rev->prev;
	
		free(lst_rev);
	}

	pom_mutex_unlock(&ct->lock);

	timer_cleanup(ce->cleanup_timer);
	
	if (ce->fwd_value)
		ptype_cleanup(ce->fwd_value);
	if (ce->rev_value)
		ptype_cleanup(ce->rev_value);

	pthread_mutex_destroy(&ce->lock);

	free(ce);

	return POM_OK;
}