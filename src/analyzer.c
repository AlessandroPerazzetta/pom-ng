/*
 *  This file is part of pom-ng.
 *  Copyright (C) 2011 Guy Martin <gmsoft@tuxicoman.be>
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

#include "analyzer.h"
#include "output.h"
#include "mod.h"
#include "input_server.h"

#include <libxml/parser.h>

static struct analyzer_reg *analyzer_head = NULL;
static pthread_mutex_t analyzer_lock = PTHREAD_MUTEX_INITIALIZER;

static struct analyzer_pload_type *analyzer_pload_types = NULL;
static struct analyzer_pload_mime_type *analyzer_pload_mime_types = NULL;

static struct analyzer_pload_class pload_class_def[ANALYZER_PLOAD_CLASS_COUNT] = {
	{ "unknown", "Unknown payload class" },
	{ "application", "Application files" },
	{ "audio", "Audio files and streams" },
	{ "image", "Images files" },
	{ "video", "Video files and streams" },
	{ "document", "Document files" },

};


int analyzer_init(char *mime_type_database) {

	xmlDocPtr doc;
	xmlNodePtr root, cur;

	doc = xmlParseFile(mime_type_database);

	if (!doc) {
		pomlog(POMLOG_ERR "Error while parsing mime type databse %s", mime_type_database);
		return POM_ERR;
	}

	root = xmlDocGetRootElement(doc);
	
	if (!root) {
		pomlog(POMLOG_ERR "Mime type database empty");
		xmlFreeDoc(doc);
		return POM_ERR;
	}

	if (xmlStrcmp(root->name, (const xmlChar*) "mime_types")) {
		pomlog(POMLOG_ERR "The first and only node should be <mime_types> !");
		xmlFreeDoc(doc);
		return POM_ERR;
	}


	// Parse definitions
	for (cur = root->xmlChildrenNode; cur && xmlStrcmp(cur->name, (const xmlChar *) "definitions"); cur = cur->next);
	if (!cur) {
		pomlog(POMLOG_ERR "<definitions> not found in the mime-types database");
		xmlFreeDoc(doc);
		return POM_ERR;
	}


	xmlNodePtr defs = cur->xmlChildrenNode;
	for (; defs; defs = defs->next) {

		if (xmlStrcmp(defs->name, (const xmlChar *) "def"))
			continue;

		char *cls_name = (char *) xmlGetProp(defs, (const xmlChar *) "class");
		if (!cls_name) {
			pomlog(POMLOG_WARN "Class name missing for definition");
			continue;
		}

		int cls_id;
		for (cls_id = 0; cls_id < ANALYZER_PLOAD_CLASS_COUNT && strcmp(pload_class_def[cls_id].name, cls_name); cls_id++);
		if (cls_id >= ANALYZER_PLOAD_CLASS_COUNT) {
			pomlog(POMLOG_WARN "Class %s does not exists for definition", cls_name);
			xmlFree(cls_name);
			continue;
		}
		xmlFree(cls_name);

		char *name = (char *) xmlGetProp(defs, (const xmlChar *) "name");
		if (!name) {
			pomlog(POMLOG_WARN "Definition name missing");
			continue;
		}

		char *description = (char *) xmlGetProp(defs, (const xmlChar *) "description");
		if (!description) {
			pomlog(POMLOG_WARN "Description missing for definition %s", name);
			xmlFree(name);
			continue;
		}

		char *extension = (char *) xmlGetProp(defs, (const xmlChar *) "extension");
		if (!extension) {
			pomlog(POMLOG_WARN "Extension missing for definition %s", name);
			xmlFree(name);
			xmlFree(description);
			continue;
		}


		struct analyzer_pload_type *def = malloc(sizeof(struct analyzer_pload_type));
		if (!def) {
			pom_oom(sizeof(struct analyzer_pload_type));
			xmlFree(name);
			xmlFree(description);
			xmlFree(extension);
			continue;
		}
		memset(def, 0, sizeof(struct analyzer_pload_type));

		def->cls = cls_id;
		def->name = strdup(name);
		if (!def->name) {
			free(def);
			xmlFree(description);
			xmlFree(extension);
			pom_oom(strlen(name) + 1);
			xmlFree(name);
			continue;
		}
		xmlFree(name);

		def->description = strdup(description);
		if (!def->description) {
			free(def->name);
			free(def);
			xmlFree(extension);
			pom_oom(strlen(description) + 1);
			xmlFree(description);
			continue;
		}
		xmlFree(description);

		def->extension = strdup(extension);
		if (!def->extension) {
			free(def->description);
			free(def->name);
			free(def);
			pom_oom(strlen(extension) + 1);
			xmlFree(extension);
			continue;

		}
		xmlFree(extension);

		def->next = analyzer_pload_types;
		if (def->next)
			def->next->prev = def;
		analyzer_pload_types = def;

		pomlog(POMLOG_DEBUG "Registered payload type %s : class %s, extension .%s", def->name, pload_class_def[def->cls].name, def->extension);

	}


	// Parse mime-types
	for (cur = root->xmlChildrenNode; cur && xmlStrcmp(cur->name, (const xmlChar *) "types"); cur = cur->next);
	if (!cur) {
		pomlog(POMLOG_ERR "<types> not found in the mime-types database");
		return POM_ERR;
	}

	xmlNodePtr types = cur->xmlChildrenNode;
	for (; types; types = types->next) {

		if (xmlStrcmp(types->name, (const xmlChar *) "type"))
			continue;

		char *def = (char *) xmlGetProp(types, (const xmlChar *) "def");
		if (!def) {
			pomlog(POMLOG_WARN "Type definition missing");
			continue;
		}

		struct analyzer_pload_type *defs = analyzer_pload_types;
		for (; defs && strcmp(defs->name, def); defs = defs->next);
		if (!defs) {
			pomlog(POMLOG_WARN "Definition %s not known", def);
			xmlFree(def);
			continue;
		}
		xmlFree(def);

		char *value = (char *) xmlNodeListGetString(doc, types->xmlChildrenNode, 1);
		if (!value) {
			pomlog(POMLOG_WARN "Empty mime type");
			continue;
		}

		struct analyzer_pload_mime_type *type = malloc(sizeof(struct analyzer_pload_mime_type));
		if (!type) {
			xmlFree(value);
			pom_oom(sizeof(struct analyzer_pload_mime_type));
			continue;
		}
		memset(type, 0, sizeof(struct analyzer_pload_mime_type));
		type->type = defs;
		type->name = strdup(value);
		if (!type->name) {
			pom_oom(strlen(value));
			xmlFree(value);
			continue;
		}
		xmlFree(value);

		type->next = analyzer_pload_mime_types;
		if (type->next)
			type->next->prev = type;
		analyzer_pload_mime_types = type;

		pomlog(POMLOG_DEBUG "Mime type %s registered as %s", type->name, type->type->name);

	}

	xmlFreeDoc(doc);

	return POM_OK;


}

int analyzer_register(struct analyzer_reg_info *reg_info) {

	if (reg_info->api_ver != ANALYZER_API_VER) {
		pomlog(POMLOG_ERR "Cannot register analyzer as API version differ : expected %u got %u", ANALYZER_API_VER, reg_info->api_ver);
		return POM_ERR;
	}

	if (input_server_is_current_process()) {
		pomlog(POMLOG_DEBUG "Not loading analyzer %s in the input process", reg_info->name);
		return POM_OK;
	}

	pom_mutex_lock(&analyzer_lock);

	// Allocate the analyzer
	struct analyzer_reg *analyzer = malloc(sizeof(struct analyzer_reg));
	if (!analyzer) {
		pom_mutex_unlock(&analyzer_lock);
		pom_oom(sizeof(struct analyzer_reg));
		return POM_ERR;
	}
	memset(analyzer, 0, sizeof(struct analyzer_reg));
	analyzer->info = reg_info;

	// Update the analyzer pointer for the events
	int i;
	for (i = 0; reg_info->events[i].name; i++)
		reg_info->events[i].analyzer = analyzer;

	if (reg_info->init) {
		if (reg_info->init(analyzer) != POM_OK) {
			pom_mutex_unlock(&analyzer_lock);
			free(analyzer);
			pomlog(POMLOG_ERR "Error while initializing analyzer %s", reg_info->name);
			return POM_ERR;
		}
	}

	analyzer->next = analyzer_head;
	if (analyzer->next)
		analyzer->next->prev = analyzer;
	analyzer_head = analyzer;
	pom_mutex_unlock(&analyzer_lock);
	
	mod_refcount_inc(reg_info->mod);

	return POM_OK;
}

int analyzer_unregister(char *name) {

	pom_mutex_lock(&analyzer_lock);
	struct analyzer_reg *tmp;
	for (tmp = analyzer_head; tmp && strcmp(tmp->info->name, name); tmp = tmp->next);

	if (!tmp) {
		pom_mutex_unlock(&analyzer_lock);
		pomlog(POMLOG_DEBUG "Analyzer %s is not registered, cannot unregister it", name);
		return POM_OK;
	}

	if (tmp->info->cleanup) {
		if (tmp->info->cleanup(tmp) != POM_OK) {
			pomlog(POMLOG_ERR "Error while cleaning up analyzer %s", name);
			return POM_ERR;
		}
	}

	if (tmp->prev)
		tmp->prev->next = tmp->next;
	else
		analyzer_head = tmp->next;

	if (tmp->next)
		tmp->next->prev = tmp->prev;

	mod_refcount_dec(tmp->info->mod);
	
	free(tmp);

	pom_mutex_unlock(&analyzer_lock);

	return POM_OK;
}

int analyzer_cleanup() {
	
	pom_mutex_lock(&analyzer_lock);

	while (analyzer_head) {

		struct analyzer_reg *tmp = analyzer_head;
		analyzer_head = tmp->next;

		if (tmp->info->cleanup) {
			if (tmp->info->cleanup(tmp))
				pomlog(POMLOG_WARN "Error while cleaning up analyzer %s", tmp->info->name);
		}

		mod_refcount_dec(tmp->info->mod);

		free(tmp);
	}

	pom_mutex_unlock(&analyzer_lock);

	while (analyzer_pload_types) {
		struct analyzer_pload_type *tmp = analyzer_pload_types;
		analyzer_pload_types = tmp->next;
		free(tmp->name);
		free(tmp->description);
		free(tmp->extension);
		free(tmp);
	}

	while (analyzer_pload_mime_types) {
		struct analyzer_pload_mime_type *tmp = analyzer_pload_mime_types;
		analyzer_pload_mime_types = tmp->next;
		free(tmp->name);
		free(tmp);
	}


	return POM_OK;

}


struct analyzer_event_reg *analyzer_event_get(char *name) {

	struct analyzer_reg *tmp;
	for (tmp = analyzer_head; tmp; tmp = tmp->next) {

		struct analyzer_event_reg *evt = tmp->info->events;
		unsigned int i;
		for (i = 0; evt[i].name; i++) {
			if (!strcmp(name, evt[i].name))
				return &evt[i];
		}
	}

	return NULL;
}



int analyzer_event_register_listener(struct analyzer_event_reg *evt, struct analyzer_event_listener *listener) {

	if (!evt->listeners && evt->listeners_notify) {
		// Notify the analyzer that the event has a listener now
		if (evt->listeners_notify(evt->analyzer, evt, 1) != POM_OK) {
			pomlog(POMLOG_ERR "Error while notifying the analyzer %s about new listeners for event %s", evt->analyzer->info->name, evt->name);
			return POM_ERR;
		}
	}

	struct analyzer_event_listener_list *listener_list = malloc(sizeof(struct analyzer_event_listener_list));
	if (!listener_list) {
		pom_oom(sizeof(struct analyzer_event_listener_list));
		return POM_ERR;
	}
	memset(listener_list, 0, sizeof(struct analyzer_event_listener_list));

	listener_list->listener = listener;
	listener_list->next = evt->listeners;
	if (listener_list->next)
		listener_list->next->prev = listener_list;
	evt->listeners = listener_list;

	return POM_OK;
}

int analyzer_event_unregister_listener(struct analyzer_event_reg *evt, char *listener_name) {

	struct analyzer_event_listener_list *tmp = evt->listeners;

	for (; tmp && strcmp(tmp->listener->name, listener_name); tmp = tmp->next);

	if (!tmp) {
		pomlog(POMLOG_ERR "Listener %s is not registered to event %s", listener_name, evt->name);
		return POM_ERR;
	}

	if (tmp->next)
		tmp->next->prev = tmp->prev;
	if (tmp->prev)
		tmp->prev->next = tmp->next;
	else
		evt->listeners = tmp->next;
	
	free(tmp);

	if (!evt->listeners && evt->listeners_notify) {
		// Notify the analyzer that the event has a listener now
		if (evt->listeners_notify(evt->analyzer, evt, 0) != POM_OK) {
			pomlog(POMLOG_ERR "Error while notifying the analyzer %s about lack of listeners for event %s", evt->analyzer->info->name, evt->name);
			return POM_ERR;
		}
	}

	return POM_OK;
}


int analyzer_event_process(struct analyzer_event *evt) {

	struct analyzer_event_listener_list *tmp = evt->info->listeners;
	while (tmp) {
		if (tmp->listener->process(tmp->listener->obj, evt) != POM_OK) {
			pomlog(POMLOG_ERR "Error while processing event %s for listener %s", evt->info->name, tmp->listener->name);
			return POM_ERR;
		}
		tmp = tmp->next;
	}

	return POM_OK;

}


struct analyzer_pload_buffer *analyzer_pload_buffer_alloc(char *content_type, size_t expected_size) {

	struct analyzer_pload_buffer *pload = malloc(sizeof(struct analyzer_pload_buffer));
	if (!pload) {
		pom_oom(sizeof(struct analyzer_pload_buffer));
		return NULL;
	}
	memset(pload, 0, sizeof(struct analyzer_pload_buffer));

	pomlog(POMLOG_DEBUG "Got new pload of type %s", content_type);

	if (expected_size > 0) {

		pload->expected_size = expected_size;

		// FIXME improve this if size if bigger than a page or so
	
		pload->buff = malloc(expected_size);
		if (!pload->buff) {
			pom_oom(expected_size);
			free(pload);
			return NULL;
		}

		pload->buff_size = expected_size;
	}

	return pload;

}


int analyzer_pload_buffer_append(struct analyzer_pload_buffer *pload, void *data, size_t size) {

	if (pload->expected_size && (pload->expected_size < pload->buff_pos + size)) {
		pomlog(POMLOG_DEBUG "Pload larger than expected size. Dropping");
		return POM_OK;
	}

	if (pload->buff_size < pload->buff_pos + size) {
		// Buffer is too small, add a page
		long pagesize = sysconf(_SC_PAGESIZE);
		if (!pagesize)
			pagesize = 4096;

		size_t new_size = (size_t) (pload->buff + pagesize);

		void *new_buff = realloc(pload->buff, new_size);
		if (!new_buff) {
			pom_oom(new_size);
			pomlog(POMLOG_ERR "Could not allocate enough memory to hold the buffer");
			return POM_ERR;
		}

		pload->buff = new_buff;
		pload->buff_size = new_size;
	}
	
	memcpy(pload->buff + pload->buff_pos, data, size);

	return POM_OK;

}


int analyzer_pload_buffer_cleanup(struct analyzer_pload_buffer *pload) {

	if (pload->buff)
		free(pload->buff);
	free(pload);

	return POM_OK;
}