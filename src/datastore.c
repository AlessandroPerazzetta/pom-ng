/*
 *  This file is part of pom-ng.
 *  Copyright (C) 2012 Guy Martin <gmsoft@tuxicoman.be>
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


#include "common.h"
#include "registry.h"
#include "datastore.h"
#include "mod.h"

#include <pom-ng/datastore.h>
#include <pom-ng/ptype_bool.h>
#include <pom-ng/ptype_uint16.h>
#include <pom-ng/ptype_uint64.h>
#include <pom-ng/ptype_string.h>

static struct registry_class *datastore_registry_class = NULL;
static struct datastore_reg *datastore_reg_head = NULL;
static struct datastore *datastore_head = NULL;

int datastore_init() {

	datastore_registry_class = registry_add_class(DATASTORE_REGISTRY);
	if (!datastore_registry_class)
		return POM_ERR;

	datastore_registry_class->instance_add = datastore_instance_add;
	datastore_registry_class->instance_remove = datastore_instance_remove;

	return POM_OK;

}

int datastore_cleanup() {

	while (datastore_head) {
		if (datastore_instance_remove(datastore_head->reg_instance) != POM_OK)
			return POM_ERR;
	}

	while (datastore_reg_head) {
		struct datastore_reg *tmp = datastore_reg_head;
		datastore_reg_head = tmp->next;
		mod_refcount_dec(tmp->module);
		free(tmp);
	}

	if (datastore_registry_class)
		registry_remove_class(datastore_registry_class);
	datastore_registry_class = NULL;

	return POM_OK;

}

int datastore_register(struct datastore_reg_info *reg_info) {

	pomlog(POMLOG_DEBUG "Registering datastore %s", reg_info->name);


	struct datastore_reg *tmp;
	for (tmp = datastore_reg_head; tmp && strcmp(tmp->info->name, reg_info->name); tmp = tmp->next);
	if (tmp) {
		pomlog(POMLOG_ERR "Datastore %s already registered", reg_info->name);
		return POM_ERR;
	}

	struct datastore_reg *reg = malloc(sizeof(struct datastore_reg));
	if (!reg) {
		pom_oom(sizeof(struct datastore_reg));
		return POM_ERR;
	}
	memset(reg, 0, sizeof(struct datastore_reg));

	reg->info = reg_info;
	mod_refcount_inc(reg_info->mod);
	reg->module = reg_info->mod;

	reg->next = datastore_reg_head;
	datastore_reg_head = reg;
	if (reg->next)
		reg->next->prev = reg;

	return POM_OK;

}

int datastore_unregister(char *name) {
	
	struct datastore_reg *reg;

	for (reg = datastore_reg_head; reg && strcmp(reg->info->name, name); reg = reg->next);
	if (!reg)
		return POM_OK;

	if (reg->prev)
		reg->prev->next = reg->next;
	else
		datastore_reg_head = reg->next;

	if (reg->next)
		reg->next->prev = reg->prev;

	reg->next = NULL;
	reg->prev = NULL;

	mod_refcount_dec(reg->module);

	free(reg);

	return POM_OK;

}

int datastore_instance_add(char *type, char *name) {

	struct datastore_reg *reg;
	for (reg = datastore_reg_head; reg && strcmp(reg->info->name, type); reg = reg->next);

	if (!reg) {
		pomlog(POMLOG_ERR "Datastore type %s does not esists", type);
		return POM_ERR;
	}

	struct datastore *res = malloc(sizeof(struct datastore));
	if (!res) {
		pom_oom(sizeof(struct datastore));
		return POM_ERR;
	}
	memset(res, 0, sizeof(struct datastore));

	// Create a new recursive lock
	pthread_mutexattr_t attr;
	if (pthread_mutexattr_init(&attr)) {
		pomlog(POMLOG_ERR "Error while initializing the conntrack mutex attribute");
		free(res);
		return POM_ERR;
	}

	if (pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE)) {
		pomlog(POMLOG_ERR "Error while setting conntrack mutex attribute to recursive");
		pthread_mutexattr_destroy(&attr);
		free(res);
		return POM_ERR;
	}

	if (pthread_mutex_init(&res->lock, &attr)) {
		pomlog(POMLOG_ERR "Error while initializing the datastore lock : %s", pom_strerror(errno));
		free(res);
		return POM_ERR;
	}

	pthread_mutexattr_destroy(&attr);

	res->reg = reg;
	res->name = strdup(name);
	if (!res->name) {
		free(res);
		pom_oom(strlen(name) + 1);
		goto err;
	}


	res->reg_instance = registry_add_instance(datastore_registry_class, name);

	if (!res->reg_instance)
		goto err;

	struct ptype *datastore_type = ptype_alloc("string");
	if (!datastore_type)
		goto err;

	struct registry_param *type_param = registry_new_param("type", type, datastore_type, "Type of the datastore", REGISTRY_PARAM_FLAG_CLEANUP_VAL | REGISTRY_PARAM_FLAG_IMMUTABLE);
	if (!type_param) {
		ptype_cleanup(datastore_type);
		goto err;
	}

	if (registry_instance_add_param(res->reg_instance, type_param) != POM_OK) {
		registry_cleanup_param(type_param);
		ptype_cleanup(datastore_type);
		goto err;
	}

	res->reg_instance->priv = res;

	if (registry_uid_create(res->reg_instance) != POM_OK)
		goto err;

	if (reg->info->init) {
		if (reg->info->init(res) != POM_OK) {
			pomlog(POMLOG_ERR "Error while initializing the datastore %s", name);
			goto err;
		}
	}

	res->next = datastore_head;
	if (res->next)
		res->next->prev = res;

	datastore_head = res;

	return POM_OK;

err:

	pthread_mutex_destroy(&res->lock);

	if (res->name)
		free(res->name);

	if (res->reg_instance)
		registry_remove_instance(res->reg_instance);

	free(res);

	return POM_ERR;

}


int datastore_instance_remove(struct registry_instance *ri) {
	
	struct datastore *d = ri->priv;

	if (datastore_close(d) != POM_OK) {
		pomlog(POMLOG_ERR "Error while closing the datastore");
		return POM_ERR;
	}

	if (d->reg->info->cleanup) {
		if (d->reg->info->cleanup(d) != POM_OK) {
			pomlog(POMLOG_ERR "Error while cleaning up the datastore");
			return POM_ERR;
		}
	}

	pthread_mutex_destroy(&d->lock);

	free(d->name);
	
	if (d->prev)
		d->prev->next = d->next;
	else
		datastore_head = d->next;

	if (d->next)
		d->next->prev = d->prev;

	free(d);

	return POM_OK;
}

int datastore_open(struct datastore *d) {

	if (!d)
		return POM_ERR;

	pom_mutex_lock(&d->lock);

	if (d->open) {
		pom_mutex_unlock(&d->lock);
		return POM_OK;
	}

	if (d->reg->info->open && d->reg->info->open(d) != POM_OK) {
		pom_mutex_unlock(&d->lock);
		pomlog(POMLOG_ERR "Error while opening datastore %s", d->reg->info->name);
		return POM_ERR;
	}


	struct ptype *dsid = NULL;
	struct datavalue_template *ds_template = NULL;

	// Allocate the dataset_db

	static struct datavalue_template dataset_db_template[] = {
		{ .name = "name", .type = "string" }, // Name of the dataset
		{ .name = "description", .type = "string" }, // Description of the dataset
		{ 0 }
	};

	
	d->dataset_db = datastore_dataset_alloc(d, dataset_db_template, DATASTORE_DATASET_TABLE);
	if (!d->dataset_db)
		goto err;

	d->dataset_db_query = datastore_dataset_query_alloc(d->dataset_db);
	
	if (!d->dataset_db_query)
		goto err;


	// Allocate the dataset_schema
	
	static struct datavalue_template dataset_schema_template[] = {
		{ .name = "dataset_id", .type = "uint64" },
		{ .name = "name", .type = "string" },
		{ .name = "type", .type = "string" },
		{ .name = "field_id", .type = "uint16" },
		{ 0 }
	};

	d->dataset_schema = datastore_dataset_alloc(d, dataset_schema_template, DATASTORE_DATASET_SCHEMA_TABLE);
	if (!d->dataset_schema)
		goto err;

	d->dataset_schema_query = datastore_dataset_query_alloc(d->dataset_schema);
	if (!d->dataset_schema_query)
		goto err;

	// Add the dsdb and dsschema datasets to the list of datasets
	
	d->dataset_db->next = d->dataset_schema;
	d->dataset_schema->prev = d->dataset_db;
	d->datasets = d->dataset_db;



	// Fetch the existings datasets
	dsid = ptype_alloc("uint64");
	if (!dsid)
		goto err;

	int found = 0;

	while (1) {
		int res = datastore_dataset_read(d->dataset_db_query);
		if (res == DATASET_QUERY_OK) {
			found = 1;
			break;
		} else if (res == DATASET_QUERY_ERR) {
			break;
		} else if (res == DATASET_QUERY_DATASTORE_ERR) {
			goto err;
		}

		if (d->dataset_db_query->values[0].is_null) {
			pomlog(POMLOG_ERR "Dataset name is NULL");
			goto err;
		}

		PTYPE_UINT64_SETVAL(dsid, d->dataset_db_query->data_id);
	

		// Set read condition
		datastore_dataset_query_set_condition(d->dataset_schema_query, 0, PTYPE_OP_EQ, dsid);

		unsigned int datacount = 0;

		while (1) {
			res = datastore_dataset_read(d->dataset_schema_query);
			if (res == DATASET_QUERY_OK) {
				found = 1;
				break;
			} else if (res == DATASET_QUERY_ERR) {
				goto err;
			} else if (res == DATASET_QUERY_DATASTORE_ERR) {
				goto err;
			}
			struct datavalue_template *tmp_template = realloc(ds_template, sizeof(struct datavalue_template) * (datacount + 2));
			if (!tmp_template) {
				pom_oom(sizeof(struct datavalue_template) * (datacount + 2));
				goto err;
			}
			ds_template = tmp_template;
			memset(&tmp_template[datacount], 0, sizeof(struct datavalue_template) * 2);

			char *name = PTYPE_STRING_GETVAL(d->dataset_schema_query->values[1].value);
			if (d->dataset_schema_query->values[1].is_null) {
				pomlog(POMLOG_ERR "NULL value for template entry name");
				goto err;
			}
			ds_template[datacount].name = strdup(name);
			if (!ds_template[datacount].name) {
				pom_oom(strlen(name) + 1);
				goto err;
			}

			char *type = PTYPE_STRING_GETVAL(d->dataset_schema_query->values[2].value);
			if (d->dataset_schema_query->values[2].is_null) {
				pomlog(POMLOG_ERR "NULL value for template entry type");
				goto err;
			}
			ds_template[datacount].type = strdup(type);
			if (!ds_template[datacount].type)
				goto err;

			datacount++;

		}


		struct dataset *ds = datastore_dataset_alloc(d, ds_template, PTYPE_STRING_GETVAL(d->dataset_db_query->values[0].value));
		if (!ds)
			goto err;

		ds->next = d->datasets;
		if (ds->next)
			ds->next->prev = ds;

		d->datasets = ds;

	}

	ptype_cleanup(dsid);
	dsid = NULL;
	ds_template = NULL;

	if (!found) {
		if (datastore_dataset_create(d->dataset_db) != POM_OK)
			goto err;

		if (datastore_dataset_create(d->dataset_schema) != POM_OK) {
			// datastore_dataset_destroy(dsdb);
			goto err;
		}

	}

	d->open = 1;
	
	pom_mutex_unlock(&d->lock);

	pomlog("Datastore %s opened", d->reg->info->name);

	return POM_OK;

err:
	
	if (d->dataset_db) {
		if (d->reg->info->dataset_cleanup)
			d->reg->info->dataset_cleanup(d->dataset_db);
		d->dataset_db = NULL;
	}

	if (d->dataset_schema) {
		if (d->reg->info->dataset_cleanup)
			d->reg->info->dataset_cleanup(d->dataset_schema);
		d->dataset_schema = NULL;
	}

	if (d->dataset_db_query) {
		datastore_dataset_query_cleanup(d->dataset_db_query);
		d->dataset_db_query = NULL;
	}

	if (d->dataset_schema_query) {
		datastore_dataset_query_cleanup(d->dataset_schema_query);
		d->dataset_schema_query = NULL;
	}

	struct dataset *dset = NULL;
	for (dset = d->datasets; d->datasets; dset = d->datasets) {
		if (d->reg->info->dataset_cleanup && d->reg->info->dataset_cleanup(dset) != POM_OK)
			pomlog(POMLOG_WARN "Warning : error while cleaning up the dataset %s from datastore %s", dset->name, d->name);
		
		d->datasets = dset->next;

		int i;
		for (i = 0; dset->data_template[i].name; i++) {
			free(dset->data_template[i].name);
			free(dset->data_template[i].type);
		}

		if (dset != d->dataset_db && dset != d->dataset_schema) // Those two have a static template
			free(dset->data_template);

		free(dset->name);
		free(dset);
	}

	if (d->reg->info->close)
		d->reg->info->close(d);

	if (dsid)
		ptype_cleanup(dsid);

	if (ds_template) {
		int i;
		for (i = 0; ds_template[i].name || ds_template[i].type; i++) {
			if (ds_template[i].name)
				free(ds_template[i].name);

			if (ds_template[i].type)
				free(ds_template[i].type);

		}

		free(ds_template);
	}

	pom_mutex_unlock(&d->lock);

	return POM_ERR;
}

int datastore_close(struct datastore *d) {

	if (!d)
		return POM_ERR;

	pom_mutex_lock(&d->lock);

	if (!d->open) {
		pom_mutex_unlock(&d->lock);
		return POM_OK;
	}

	struct dataset* dset = d->datasets;

	// Check if all datasets are closed
	while (dset) {

		if (dset->open) {
			pomlog(POMLOG_ERR "Cannot close datastore %s as the dataset %s is still open", d->name, dset->name);
			pom_mutex_unlock(&d->lock);
			return POM_ERR;
		}
		dset = dset->next;
	}



	dset = d->datasets;

	// Cleanup our specific queries
	
	if (d->reg->info->dataset_query_cleanup) {
		if (datastore_dataset_query_cleanup(d->dataset_db_query) != POM_OK)
			pomlog(POMLOG_WARN "Warning : error while cleaning up the dataset db query");
		d->dataset_db_query = NULL;

		if (datastore_dataset_query_cleanup(d->dataset_schema_query) != POM_OK)
			pomlog(POMLOG_WARN "Warning : error while cleaning up the dataset schema query");
		d->dataset_schema_query = NULL;
	}

	// Cleanup all the datasets
	
	for (dset = d->datasets; d->datasets; dset = d->datasets) {
		if (d->reg->info->dataset_cleanup && d->reg->info->dataset_cleanup(dset) != POM_OK)
			pomlog(POMLOG_WARN "Warning : error while cleaning up the dataset %s from datastore %s", dset->name, d->name);
		
		d->datasets = dset->next;

		if (dset != d->dataset_db && dset != d->dataset_schema) { // Those two have a static template
			int i;
			for (i = 0; dset->data_template[i].name; i++) {
				free(dset->data_template[i].name);
				free(dset->data_template[i].type);
			}

			free(dset->data_template);
		}

		free(dset->name);
		free(dset);
	}

	d->dataset_db = NULL;
	d->dataset_schema = NULL;

	// Close the datastore
	
	if (d->reg->info->close && d->reg->info->close(d) != POM_OK)
		pomlog(POMLOG_WARN "Warning : error while closing the datastore");

	d->open = 0;

	pom_mutex_unlock(&d->lock);

	return POM_OK;
}

struct datastore *datastore_instance_get(char *datastore_name) {
	struct datastore *res;
	for (res = datastore_head; res && strcmp(res->name, datastore_name); res = res->next);

	return res;
}

struct dataset *datastore_dataset_open(struct datastore *d, char *name, struct datavalue_template *dt) {

	struct dataset *res = NULL;

	struct datavalue_template *new_dt = NULL;
	int i;

	pom_mutex_lock(&d->lock);

	if (!d->open) {
		if (datastore_open(d) != POM_OK) {
			pom_mutex_unlock(&d->lock);
			return NULL;
		}
	}

	for (res = d->datasets; res && strcmp(res->name, name); res = res->next);
	
	if (res) {
		if (res->open) { // Datastore found and already open
			pom_mutex_unlock(&d->lock);
			return res;
		}
		
		struct datavalue_template *flds = res->data_template;
		int i;

		for (i = 0; dt[i].name; i++) {
			if (!flds->name || strcmp(dt[i].name, flds[i].name) || strcmp(dt[i].type, flds[i].type)) {
				pom_mutex_unlock(&d->lock);
				pomlog(POMLOG_ERR "Cannot open dataset %s. Missmatch in provided vs existing fields", name);
				return NULL;
			}
		}
		
		res->open = 1;

	} else {
		pomlog("Dataset %s doesn't exists in datastore %s. Creating it ...", name, d->name);

		// Copy the template
		for (i = 0; dt[i].name; i++);
		i++;

		size_t size = sizeof(struct datavalue_template) * i;
		struct datavalue_template *new_dt = malloc(size);
		if (!new_dt) {
			pom_oom(size);
			return NULL;
		}
		memset(new_dt, 0, size);
		
		for (i = 0; dt[i].name; i++) {

			new_dt[i].name = strdup(dt[i].name);
			if (!new_dt[i].name) {
				pom_oom(strlen(dt[i].name) + 1);
				goto err;
			}

			new_dt[i].type = strdup(dt[i].type);
			if (!new_dt[i].type) {
				pom_oom(strlen(dt[i].type) + 1);
				goto err;
			}
		}

		// Allocate the new dataset
		res = datastore_dataset_alloc(d, new_dt, name);
		if (!res)
			goto err;

		if (datastore_dataset_create(res) != POM_OK)
			goto err;


		// Add it in the database
		PTYPE_STRING_SETVAL(d->dataset_db_query->values[0].value, name);
		d->dataset_db_query->values[0].is_null = 0;
		d->dataset_db_query->values[1].is_null = 1;
		if (datastore_dataset_write(d->dataset_db_query) != POM_OK)
			goto err;

		res->dataset_id = d->dataset_db_query->data_id;

		for (i = 0; dt[i].name; i++) {
			PTYPE_UINT64_SETVAL(d->dataset_schema_query->values[0].value, res->dataset_id);
			d->dataset_schema_query->values[0].is_null = 0;
			PTYPE_STRING_SETVAL(d->dataset_schema_query->values[1].value, dt[i].name);
			d->dataset_schema_query->values[1].is_null = 0;
			PTYPE_STRING_SETVAL(d->dataset_schema_query->values[2].value, dt[i].type);
			d->dataset_schema_query->values[2].is_null = 0;
			PTYPE_UINT16_SETVAL(d->dataset_schema_query->values[3].value, i);
			d->dataset_schema_query->values[3].is_null = 0;


			if (datastore_dataset_write(d->dataset_schema_query) != POM_OK)
				goto err;

		}

		// Add it in the list of datasets
		res->next = d->datasets;
		if (res->next)
			res->next->prev = res;
		d->datasets = res;

	}

	pom_mutex_unlock(&d->lock);

	pomlog(POMLOG_DEBUG "Dataset %s in datastore %s opened", res->name, d->name);

	return res;

err:

	pom_mutex_unlock(&d->lock);

	if (new_dt) {
		for (i = 0; new_dt[i].name || new_dt[i].type; i++) {
			if (new_dt[i].name)
				free(new_dt[i].name);
			if (new_dt[i].type)
				free(new_dt[i].type);
		}
		free(new_dt);
	}

	if (!res)
		return NULL;

	//datastore_dataset_destroy(res);

	datastore_dataset_cleanup(res);

	// FIXME remove the datastore from the db if it was already added
	// or implement begin/commit/rollback
	
	return NULL;

}

int datastore_dataset_close(struct dataset *ds) {

	struct datastore *d = ds->dstore;

	pom_mutex_lock(&d->lock);
	ds->open = 0;
	pom_mutex_unlock(&d->lock);

	return POM_OK;
}

struct dataset *datastore_dataset_alloc(struct datastore *d, struct datavalue_template *dt, char *name) {

	struct dataset *ds = malloc(sizeof(struct dataset));
	if (!ds) {
		pom_oom(sizeof(struct dataset));
		return NULL;
	}
	memset(ds, 0, sizeof(struct dataset));

	ds->data_template = dt;
	
	ds->name = strdup(name);
	if (!ds->name) {
		pom_oom(strlen(name) + 1);
		goto err;
	}

	ds->dstore = d;

	if (d->reg->info->dataset_alloc) {
		if (d->reg->info->dataset_alloc(ds) != POM_OK)
			goto err;
	}

	return ds;

err:
	if (ds->name)
		free(ds->name);

	free(ds);

	return NULL;
}

int datastore_dataset_cleanup(struct dataset *ds) {

	struct datastore *d = ds->dstore;
	
	if (d->reg->info->dataset_cleanup) {
		if (d->reg->info->dataset_cleanup(ds) != POM_OK)
			return POM_ERR;
	}

	free(ds->name);
	free(ds);

	return POM_OK;

}

int datastore_dataset_create(struct dataset *ds) {

	struct datastore *d = ds->dstore;

	int res = DATASET_QUERY_OK;

	if (d->reg->info->dataset_create)
		res = d->reg->info->dataset_create(ds);

	if (res != DATASET_QUERY_OK) {
		// TODO error notify
		return POM_ERR;
	}

	return POM_OK;

}

int datastore_dataset_read(struct dataset_query *dsq) {

	struct datastore *d = dsq->ds->dstore;

	if (!dsq->prepared) {
		if (d->reg->info->dataset_query_prepare) {
			int res = d->reg->info->dataset_query_prepare(dsq);
			if (res != DATASET_QUERY_OK)
				return res;
		}
		
		dsq->prepared = 1;
	}

	// FIXME handle error

	return d->reg->info->dataset_read(dsq);

}

int datastore_dataset_write(struct dataset_query *dsq) {

	struct datastore *d = dsq->ds->dstore;

	if (!dsq->prepared) {
		if (d->reg->info->dataset_query_prepare) {
			int res = d->reg->info->dataset_query_prepare(dsq);
			if (res != DATASET_QUERY_OK)
				return res;
		}
		
		dsq->prepared = 1;
	}
	
	// FIXME handle error
	
	return d->reg->info->dataset_write(dsq);

}

struct dataset_query *datastore_dataset_query_alloc(struct dataset *ds) {

	struct dataset_query *query = malloc(sizeof(struct dataset_query));
	if (!query) {
		pom_oom(sizeof(struct dataset_query));
		return NULL;
	}
	memset(query, 0, sizeof(struct dataset_query));
	query->ds = ds;

	int datacount;
	for (datacount = 0; ds->data_template[datacount].name; datacount++);

	query->values = malloc(sizeof(struct datavalue) * datacount);
	if (!query->values) {
		free(query);
		pom_oom(sizeof(struct datavalue) * datacount);
		return NULL;
	}
	memset(query->values, 0, sizeof(struct datavalue) * datacount);

	int i;
	for (i = 0; i < datacount; i++) {
		query->values[i].value = ptype_alloc(ds->data_template[i].type);
		if (!query->values[i].value)
			goto err;
	}

	struct datastore *d = ds->dstore;

	if (d->reg->info->dataset_query_alloc) {
		if (d->reg->info->dataset_query_alloc(query) != POM_OK)
			goto err;
	}
	return query;

err:
	
	for (i = 0; i < datacount; i++) {
		if (query->values[i].value)
			ptype_cleanup(query->values[i].value);
	}

	free(query->values);

	free(query);
	return NULL;

}

int datastore_dataset_query_cleanup(struct dataset_query *dsq) {

	struct datastore *d = dsq->ds->dstore;
	if (d->reg->info->dataset_query_cleanup)
		d->reg->info->dataset_query_cleanup(dsq);

	int i;
	for (i = 0; dsq->ds->data_template[i].name; i++)
		ptype_cleanup(dsq->values[i].value);
	
	free(dsq->values);

	if (dsq->cond) {
		if (dsq->cond->value)
			ptype_cleanup(dsq->cond->value);
		free(dsq->cond);
	}

	free(dsq);
	return POM_OK;
}

int datastore_dataset_query_set_condition(struct dataset_query *dsq, short field_id, int ptype_op, struct ptype *value) {

	dsq->prepared = 0;
	
	if (!dsq->cond) {
		dsq->cond = malloc(sizeof(struct datavalue_condition));
		if (!dsq->cond) {
			pom_oom(sizeof(struct datavalue_condition));
			return POM_ERR;
		}
		memset(dsq->cond, 0, sizeof(struct datavalue_condition));
	}

	struct datavalue_condition *cond = dsq->cond;

	cond->field_id = field_id;
	cond->op = ptype_op;

	if (cond->value)
		ptype_cleanup(cond->value);
	cond->value = ptype_alloc_from(value);
	if (!cond->value) {
		free(cond);
		dsq->cond = NULL;
		return POM_ERR;
	}

	return POM_OK;
}

int datastore_dataset_query_unset_condition(struct dataset_query *dsq) {

	if (!dsq->cond)
		return POM_OK;

	if (dsq->cond->value)
		ptype_cleanup(dsq->cond->value);
	free(dsq->cond);
	dsq->cond = NULL;

	dsq->prepared = 0;

	return POM_OK;
}

