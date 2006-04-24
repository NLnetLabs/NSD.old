/*
 * options.h -- nsd.conf options definitions and prototypes
 *
 * Copyright (c) 2001-2006, NLnet Labs. All rights reserved.
 *
 * See LICENSE for the license.
 *
 */

#ifndef OPTIONS_H
#define OPTIONS_H

#include <config.h>
#include <stdarg.h>
#include "region-allocator.h"
#include "rbtree.h"
struct query;
struct dname;

typedef struct nsd_options nsd_options_t;
typedef struct zone_options zone_options_t;
typedef struct ipaddress_option ip_address_option_t;
typedef struct acl_options acl_options_t;
typedef struct key_options key_options_t;
typedef struct config_parser_state config_parser_state_t;
/*
 * Options global for nsd.
 */
struct nsd_options {
	/* options for zones, by apex, contains zone_options_t */
	rbtree_t* zone_options;

	/* list of keys defined */
	key_options_t* keys;
	size_t numkeys;
	
	/* list of ip adresses to bind to (or NULL for all) */
	ip_address_option_t* ip_addresses;

	int debug_mode;
	int ip4_only;
	int ip6_only;
	const char* database;
	const char* identity;
	const char* logfile;
	int server_count;
	int tcp_count;
	const char* pidfile;
	const char* port;
	int statistics;
	const char* chroot;
	const char* username;
	const char* zonesdir;
	const char* difffile;
	const char* xfrdfile;
	int xfrd_reload_timeout;
	
	region_type* region; 
};

struct ipaddress_option {
	ip_address_option_t* next;
	char* address;
};

/*
 * Options for a zone
 */
struct zone_options {
	/* key is dname of apex */
	rbnode_t node;

	/* is apex of the zone */
	const char* name;
	const char* zonefile;
	acl_options_t* allow_notify;
	acl_options_t* request_xfr;
	acl_options_t* notify;
	acl_options_t* provide_xfr;
};

union acl_addr_storage {
#ifdef INET6
	struct in_addr addr;
	struct in6_addr addr6;
#else
	struct in_addr addr;
#endif
};

/*
 * Access control list element
 */
struct acl_options {
	acl_options_t* next;

	/* ip address range */
	const char* ip_address_spec;
	uint8_t is_ipv6;
	unsigned int port;	/* is 0(no port) or suffix @port value */
	union acl_addr_storage addr;
	union acl_addr_storage range_mask;
	enum {
		acl_range_single = 0,	/* single adress */
		acl_range_mask = 1,	/* 10.20.30.40&255.255.255.0 */
		acl_range_subnet = 2,	/* 10.20.30.40/28 */
		acl_range_minmax = 3	/* 10.20.30.40-10.20.30.60 (mask=max) */
	} rangetype;

	/* key */
	uint8_t nokey;
	uint8_t blocked;
	const char* key_name;
	key_options_t* key_options;
};

/*
 * Key definition
 */
struct key_options {
	key_options_t* next;
	const char* name;
	const char* algorithm;
	const char* secret;
};

/*
 * Used during options parsing
 */
struct config_parser_state {
	const char* filename;
	int line;
	int errors;
	nsd_options_t* opt;
	zone_options_t* current_zone;
	key_options_t* current_key;
	ip_address_option_t* current_ip_address_option;
	acl_options_t* current_allow_notify;
	acl_options_t* current_request_xfr;
	acl_options_t* current_notify;
	acl_options_t* current_provide_xfr;
};

extern config_parser_state_t* cfg_parser;

/* region will be put in nsd_options struct. Returns empty options struct. */
nsd_options_t* nsd_options_create(region_type* region);
/* the number of zones that are configured */
static inline size_t nsd_options_num_zones(nsd_options_t* opt)
{ return opt->zone_options->count; }
/* insert a zone into the main options tree, returns 0 on error */
int nsd_options_insert_zone(nsd_options_t* opt, zone_options_t* zone);

/* parses options file. Returns false on failure */
int parse_options_file(nsd_options_t* opt, const char* file);
zone_options_t* zone_options_create(region_type* region);
/* find a zone by apex domain name, or NULL if not found. */
zone_options_t* zone_options_find(nsd_options_t* opt, const struct dname* apex);
key_options_t* key_options_create(region_type* region);
key_options_t* key_options_find(nsd_options_t* opt, const char* name);
/* tsig must be inited, adds all keys in options to tsig. */
void key_options_tsig_add(nsd_options_t* opt);

/* check acl list, acl number that matches if passed(0..), 
 * or failure (-1) if dropped */
/* the reason why (the acl) is returned too (or NULL) */
int acl_check_incoming(acl_options_t* acl, struct query* q, 
	acl_options_t** reason);
int acl_addr_matches(acl_options_t* acl, struct query* q);
int acl_key_matches(acl_options_t* acl, struct query* q);
int acl_addr_match_mask(uint32_t* a, uint32_t* b, uint32_t* mask, size_t sz);
int acl_addr_match_range(uint32_t* minval, uint32_t* x, uint32_t* maxval, size_t sz);

/* returns true if acls are both from the same host */
int acl_same_host(acl_options_t* a, acl_options_t* b);

/* see if a zone is a slave or a master zone */
int zone_is_slave(zone_options_t* opt);

/* parsing helpers */
void c_error(const char* msg);
void c_error_msg(const char* fmt, ...) ATTR_FORMAT(printf, 1, 2);

#endif /* OPTIONS_H */
