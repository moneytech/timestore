/* NOTE!!! When returning whole series each point should be represented as a time/value pair
 * for direct compatibility with Flot.  The timestamp should be in milliseconds for direct
 * compatibility with Javascript.  When generating a timestamp for a new point it should be
 * generated in UTC! */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <inttypes.h>
#include <time.h>
#include <math.h>

#include "tsdb.h"
#include "cJSON/cJSON.h"

#include "http.h"
#include "http_tsdb.h"
#include "logging.h"

#define MIME_TYPE		"application/json"

#define SCN_NODE		("/nodes/%" SCNx64)
#define SCN_NODE_TIMESTAMP	("/nodes/%" SCNx64 "/values/%" SCNi64)
#define SCN_NODE_METRIC		("/nodes/%" SCNx64 "/series/%u")
#define PRI_NODE		("/nodes/%016" PRIx64)
#define PRI_NODE_TIMESTAMP	("/nodes/%016" PRIx64 "/values/%" PRIi64)
#define PRI_NODE_METRIC		("/nodes/%016" PRIx64 "/series/%u")

/*! Maximum length of output buffer for Location and MIME headers */
#define MAX_HEADER_STRING	128

/*! Default number of points to return in a series */
/* FIXME: Make this runtime configurable */
#define DEFAULT_SERIES_NPOINTS	24

#define PROFILE_STORE	\
	struct timeval pf_start; \
	struct timeval pf_stop; \
	struct timeval pf_duration;
#define PROFILE_START	{ \
	gettimeofday(&pf_start, NULL); \
	}
#define PROFILE_END(a)	{ \
	gettimeofday(&pf_stop, NULL); \
	timersub(&pf_stop, &pf_start, &pf_duration); \
	INFO("%s took %lu.%06lu s\n", a, (unsigned long)pf_duration.tv_sec, (unsigned long)pf_duration.tv_usec); \
	}

static int post_values_value_parser(cJSON *json, unsigned int *nmetrics, double *values)
{
	cJSON *subitem = json->child;
	
	FUNCTION_TRACE;
	
	while (subitem) {
		if (*nmetrics == TSDB_MAX_METRICS) {
			ERROR("Maximum number of metrics exceeded\n");
			return -EINVAL;
		}
		switch (subitem->type) {
			case cJSON_NULL:
				values[(*nmetrics)++] = NAN;
				break;
			case cJSON_Number:
				values[(*nmetrics)++] = subitem->valuedouble;
				break;
			default:
				ERROR("values must be numeric or null\n");
				return -EINVAL;
		}
		subitem = subitem->next;
	}
	DEBUG("found values for %u metrics\n", *nmetrics);
	return 0;
}

static int post_values_data_parser(cJSON *json, int64_t *timestamp, unsigned int *nmetrics, double *values)
{
	cJSON *subitem = json->child;
	
	FUNCTION_TRACE;
	
	*nmetrics = 0;
	while (subitem) {		
		/* Look for items of interest - we don't recurse except to parse the
		 * value array */
		if (strcmp(subitem->string, "timestamp") == 0) {
			if (subitem->type != cJSON_Number) {
				ERROR("timestamp must be numeric\n");
				return -EINVAL;
			}
#ifdef HTTP_DENY_FUTURE_POST
			if (subitem->valuedouble / 1000.0 > *timestamp) {
				ERROR("timestamp in the future is forbidden\n");
				return -EACCES;
			}
#endif
			*timestamp = subitem->valuedouble / 1000.0;
		} else if (strcmp(subitem->string, "values") == 0) {
			if (post_values_value_parser(subitem, nmetrics, values) < 0) {
				return -EINVAL;
			}
		}
		subitem = subitem->next;
		
	}
	return 0;
}
	
HTTP_HANDLER(http_tsdb_get_nodes)
{
	FUNCTION_TRACE;
	
	ERROR("http_tsdb_get_nodes not supported yet\n");
	
	/* Not supported - returns 404 */
	return MHD_HTTP_NOT_FOUND;
}

HTTP_HANDLER(http_tsdb_get_node)
{
	FUNCTION_TRACE;
	
	ERROR("http_tsdb_get_node not supported yet\n");
	
	return MHD_HTTP_NOT_FOUND;
}

HTTP_HANDLER(http_tsdb_create_node)
{
	FUNCTION_TRACE;
	
	ERROR("http_tsdb_create_node not supported yet\n");

	return MHD_HTTP_NOT_FOUND;
}

HTTP_HANDLER(http_tsdb_redirect_latest)
{
	tsdb_ctx_t *db;
	int64_t timestamp;
	uint64_t node_id;
	
	FUNCTION_TRACE;
	
	/* Extract node ID from the URL */
	if (sscanf(url, SCN_NODE, &node_id) != 1) {
		/* If the node ID part of the URL doesn't decode then treat as a 404 */
		ERROR("Invalid node\n");
		return MHD_HTTP_NOT_FOUND;
	}
	
	/* Attempt to open specified node - do not create if it doesn't exist */
	db = tsdb_open(node_id, 0, 0);
	if (db == NULL) {
		ERROR("Invalid node\n");
		return MHD_HTTP_NOT_FOUND;
	}
	
	/* Get latest time point */
	timestamp = tsdb_get_latest(db);
	tsdb_close(db);
	
	if (timestamp == TSDB_NO_TIMESTAMP) {
		/* Special case - no points in database */
		return MHD_HTTP_NOT_FOUND;
	}
	
	/* Set Location: header to redirect to specific URL */
	*location = (char*)malloc(MAX_HEADER_STRING);
	if (*location == NULL) {
		CRITICAL("Out of memory\n");
		return MHD_HTTP_INTERNAL_SERVER_ERROR;
	}
	snprintf(*location, MAX_HEADER_STRING, PRI_NODE_TIMESTAMP, node_id, timestamp);
	return MHD_HTTP_FOUND;
}

HTTP_HANDLER(http_tsdb_post_values)
{
	tsdb_ctx_t *db;
	cJSON *json;
	uint64_t node_id;
	unsigned int nmetrics;
	int64_t timestamp;
	double values[TSDB_MAX_METRICS];
	int rc;
	PROFILE_STORE;
	
	FUNCTION_TRACE;
	
	/* Extract node ID from the URL */
	if (sscanf(url, SCN_NODE, &node_id) != 1) {
		/* If the node ID part of the URL doesn't decode then treat as a 404 */
		ERROR("Invalid node\n");
		return MHD_HTTP_NOT_FOUND;
	}
	
	/* Submission timestamp defaults to current time */
	timestamp = (int64_t)time(NULL);
	
	/* Parse payload - returns 400 Bad Request on syntax error */
	json = cJSON_Parse(req_data);
	if (!json || (rc = post_values_data_parser(json, &timestamp, &nmetrics, values))) {
		ERROR("JSON error: %d\n", rc);
		return (rc == -EACCES) ? MHD_HTTP_FORBIDDEN : MHD_HTTP_BAD_REQUEST;
	}
	cJSON_Delete(json);
	
	INFO("POST point for %016" PRIx64 " at %" PRIi64 " for %u metrics\n", node_id, timestamp, nmetrics);

	/* Open specified node and validate new values */
	PROFILE_START;
	db = tsdb_open(node_id, nmetrics, 0);
	if (db == NULL) {
		ERROR("Invalid node\n");
		return MHD_HTTP_NOT_FOUND;
	}
	if (nmetrics != db->meta->nmetrics) {
		ERROR("Incorrect number of metrics provided (got %u, expected %" PRIu32 ")\n", nmetrics, db->meta->nmetrics);
		tsdb_close(db);
		return MHD_HTTP_BAD_REQUEST;
	}
	
	/* Update the database */
	if ((rc = tsdb_update_values(db, &timestamp, values)) < 0) {
		/* -ENOENT returned if timestamp is before the start of the database */
		ERROR("Update failed\n");
		tsdb_close(db);
		return (rc == -ENOENT) ? MHD_HTTP_BAD_REQUEST : MHD_HTTP_INTERNAL_SERVER_ERROR;
	}
	tsdb_close(db);
	PROFILE_END("update");
	
	/* Set Location: header to redirect to specific URL */
	*location = (char*)malloc(MAX_HEADER_STRING);
	if (*location == NULL) {
		CRITICAL("Out of memory\n");
		return MHD_HTTP_INTERNAL_SERVER_ERROR;
	}
	snprintf(*location, MAX_HEADER_STRING, PRI_NODE_TIMESTAMP, node_id, timestamp);	
	return MHD_HTTP_CREATED;
}

HTTP_HANDLER(http_tsdb_get_values)
{
	tsdb_ctx_t *db;
	int64_t timestamp, timestamp_orig;
	uint64_t node_id;
	double values[TSDB_MAX_METRICS];
	cJSON *json;	
	int rc;
	PROFILE_STORE;
	
	FUNCTION_TRACE;
	
	/* Extract node ID and timestamp from the URL */
	if (sscanf(url, SCN_NODE_TIMESTAMP, &node_id, &timestamp) != 2) {
		/* If the URL doesn't parse then treat as a 404 */
		ERROR("Invalid node or timestamp\n");
		return MHD_HTTP_NOT_FOUND;
	}
	timestamp_orig = timestamp;
	
	/* Attempt to open specified node - do not create if it doesn't exist */
	PROFILE_START;
	db = tsdb_open(node_id, 0, 0);
	if (db == NULL) {
		ERROR("Invalid node\n");
		return MHD_HTTP_NOT_FOUND;
	}
	
	/* Get values for the selected time point */
	if ((rc = tsdb_get_values(db, &timestamp, values)) < 0) {
		/* For an out of range time point we return a 404 */
		tsdb_close(db);
		ERROR("Fetch failed\n");
		return (rc == -ENOENT) ? MHD_HTTP_NOT_FOUND : MHD_HTTP_INTERNAL_SERVER_ERROR;
	}
	
#ifdef HTTP_TSDB_ROUND_TIMESTAMP_URLS
	/* If the timestamp needs rounding then redirect the client */
	if (timestamp != timestamp_orig) {
		tsdb_close(db);
		
		/* Set Location: header to redirect to specific URL */
		*location = (char*)malloc(MAX_HEADER_STRING);
		if (*location == NULL) {
			CRITICAL("Out of memory\n");
			return MHD_HTTP_INTERNAL_SERVER_ERROR;
		}
		snprintf(*location, MAX_HEADER_STRING, PRI_NODE_TIMESTAMP, node_id, timestamp);
		return MHD_HTTP_FOUND;
	}
#endif

	/* Encode the response record */
	json = cJSON_CreateObject();
	cJSON_AddNumberToObject(json, "timestamp", timestamp * 1000);
	cJSON_AddItemToObject(json, "values", cJSON_CreateDoubleArray(values, db->meta->nmetrics));
	tsdb_close(db);
	PROFILE_END("get_values");
	
	/* Pass response back to handler and set MIME type */
	*resp_data = cJSON_Print(json);
	cJSON_Delete(json);
	DEBUG("JSON: %s\n", *resp_data);
	*resp_data_size = strlen(*resp_data);
	*mime_type = strdup(MIME_TYPE);
	return MHD_HTTP_OK;
}

// FIXME: Do this more intelligently
#define BUF_SIZE	(1024 * 1024)

HTTP_HANDLER(http_tsdb_get_series)
{
	tsdb_ctx_t *db;
	const char *param;
	uint64_t node_id;
	unsigned int metric_id, npoints = DEFAULT_SERIES_NPOINTS;
	int64_t start = TSDB_NO_TIMESTAMP, end = TSDB_NO_TIMESTAMP;
	tsdb_series_point_t *points, *pointptr;
	char *outbuf, *bufptr;
	PROFILE_STORE;
	
	FUNCTION_TRACE;
	
	/* Extract node and metric IDs from the URL */
	if (sscanf(url, SCN_NODE_METRIC, &node_id, &metric_id) != 2) {
		/* If the URL doesn't parse then treat as a 404 */
		ERROR("Invalid series\n");
		return MHD_HTTP_NOT_FOUND;
	}
	
	/* Parse query parameters */
	param = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "start");
	if (param) {
		sscanf(param, "%" SCNi64, &start);
	}
	param = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "end");
	if (param) {
		sscanf(param, "%" SCNi64, &end);
	}
	param = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "npoints");
	if (param) {
		sscanf(param, "%u", &npoints);
	}
	DEBUG("start = %" PRIi64 " end = %" PRIi64 " npoints = %u\n", start, end, npoints);
	
	/* Allocate output buffer */
	PROFILE_START;
	pointptr = points = (tsdb_series_point_t*)malloc(sizeof(tsdb_series_point_t) * npoints);
	if (points == NULL) {
		CRITICAL("Out of memory\n");
		return MHD_HTTP_INTERNAL_SERVER_ERROR;
	}
	
	/* Fetch the requested series */
	db = tsdb_open(node_id, 0, 0);
	if (db == NULL) {
		ERROR("Invalid node\n");
		return MHD_HTTP_NOT_FOUND;
	}
	
	if ((npoints = tsdb_get_series(db, metric_id, start, end, npoints, 0, points)) < 0) {
		/* Will fail with -ENOENT if the metric ID is invalid - 404 */
		tsdb_close(db);
		ERROR("Fetch failed\n");
		return (npoints == -ENOENT) ? MHD_HTTP_NOT_FOUND : MHD_HTTP_INTERNAL_SERVER_ERROR;
	}
	tsdb_close(db);
	
	/* Encode the response record - this is a 2D array and easier to serialise without
	 * using CJSON */
	bufptr = outbuf = (char*)malloc(BUF_SIZE);
	if (outbuf == NULL) {
		CRITICAL("Out of memory\n");
		return MHD_HTTP_INTERNAL_SERVER_ERROR;
	}
	
	bufptr += snprintf(bufptr, outbuf + BUF_SIZE - bufptr, "[");
	while (npoints--) {
		bufptr += snprintf(bufptr, outbuf + BUF_SIZE - bufptr, "[ %" PRIi64 ", %f ]%s",
			pointptr->timestamp * 1000, /* return in ms for JavaScript */
			pointptr->value,
			npoints ? ", " : "");
		pointptr++;
	}
	bufptr += snprintf(bufptr, outbuf + BUF_SIZE - bufptr, "]");
	free(points);
	PROFILE_END("get_series");
	
	/* Pass response back to handler and set MIME type */
	*resp_data = outbuf;
	DEBUG("JSON: %s\n", *resp_data);
	*resp_data_size = (unsigned int)(bufptr - outbuf);
	*mime_type = strdup(MIME_TYPE);
	return MHD_HTTP_OK;
}
