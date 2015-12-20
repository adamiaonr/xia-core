#include "rid.h"

/*
** write the message to stdout unless in quiet mode
*/
void say(const char * fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	vprintf(fmt, args);
	va_end(args);
}

/*
** always write the message to stdout
*/
void warn(const char * fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	vfprintf(stdout, fmt, args);
	va_end(args);
}

/*
** write the message to stdout, and exit the app
*/
void die(int ecode, const char * fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	vfprintf(stdout, fmt, args);
	va_end(args);
	fprintf(stdout, "[rid]: exiting\n");
	exit(ecode);
}

char * name_to_rid(char * name)
{
	// by default, we consider all possible prefix lengths
	// TODO: implement a non-default behavior (e.g. argument specifying the
	// prefix lengths to encode)

	char * rid_string;
	size_t name_len = strlen(name);

	// transform the URL-like name into a bloom filter (BF) of size
	// m = 160 bit
	struct bloom * bloom_filter = (struct bloom *) calloc(1, sizeof(struct bloom));
	bloom_init(bloom_filter);

	// do this in order not to destroy name in strtok()
	char * name_to_mangle = (char *) calloc(PREFIX_MAX_LENGTH, sizeof(char));
	strncpy(name_to_mangle, name, strlen(name));

	// examples subprefixes for a name 'a/b/c' are 'a', 'a/b' or 'a/b/c'. 
	// we'll build all possible subprefixes and encode them in a BF
	char * subprefix = (char *) calloc(PREFIX_MAX_LENGTH, sizeof(char));
	size_t subprefix_len = 0;
	// // holds the end index of each subprefix in name_to_mangle, according 
	// // to subprefix size
	// int * subprefix_index = (int *) calloc(PREFIX_MAX_COUNT, sizeof(int));
	int subprefix_count = 0;

	// extract the first subprefix (i.e. from start of prefix till the first
	// occurrence of '/')
	char * name_element;
	name_element = strtok(name_to_mangle, PREFIX_DELIM);

	int bf_collisions = 0;

	while (name_element != NULL && !(subprefix_count > PREFIX_MAX_COUNT)) {

		// concatenate the next name_element to the current subprefix
		subprefix = strncat(
				subprefix,
				(const char *) name_element,
				(PREFIX_MAX_LENGTH - strlen(subprefix) - 1));

		subprefix_len = strlen(subprefix);

		// TODO: shall we include '/' for the BF encoding part?
		// check if this is the final token
		if (subprefix_len < name_len) {

			subprefix = strncat(
					subprefix,
					PREFIX_DELIM,
					(PREFIX_MAX_LENGTH - subprefix_len - 1));

			subprefix_len++;
		}

		// // update the index in prefix at which the current subprefix ends
		// subprefix_index[subprefix_count++] = subprefix_len;
		subprefix_count++;

		// add the current subprefix to the BF
		//printf("adding prefix %s, size %d\n", subPrefix, subPrefixLen);
		if(bloom_add(bloom_filter, subprefix, subprefix_len)) { 
			bf_collisions++; 
		}

		// capture the next name_element
		name_element = strtok(NULL, PREFIX_DELIM);
	}

	//bloom_print(bloom_filter);

	rid_string = to_rid_string(bloom_filter->bf);

	free(bloom_filter);
	free(subprefix);
	free(name_to_mangle);

	return rid_string;
}

char * to_rid_string(unsigned char * xid) 
{

	int i = 0, rid_dag_prefix_len = strlen(RID_STR_PREFIX);

	char * rid_string = (char *) calloc(RID_STR_SIZE, sizeof(char));

	// add the initial prefix to rid_string, i.e. "RID:"
	strncpy(rid_string, RID_STR_PREFIX, rid_dag_prefix_len);

	// add the actual XID to rid_string.

	// note we build the rid_string in the following format:
	//	"RID:" + xid[0] + xid[1] + ... + xid[XID_SIZE - 1]
	// is this correct? shouldn't it be:
	//	"RID:" + xid[XID_SIZE - 1] + xid[XID_SIZE - 2] + ... + xid[0]
	// guess that, if an analogous to_rid_byte() decodes the string in 
	// a simila way (i.e. considering xid[0] right after "RID:") then 
	// there's no problem...
	for (i = 0; i < XID_SIZE; i++) {

		// add xid[i], formatted to %02x every 2 byte, with an offset of
		// rid_dag_prefix_len, so that xid[i] start being written after the
		// "RID:" part
		snprintf(
				rid_string + (i * 2) + rid_dag_prefix_len,
				RID_STR_SIZE,
				"%02x",
				(int) (xid[i]));
	}

	return rid_string;
}

void to_rid_addr(
	char * rid_string, 
	char * ad, 
	char * hid,
	sockaddr_x * rid_addr,
	socklen_t * rid_addr_len) 
{
	// generate the DAG Graph, which will be composed by 4 nodes, like
	// this:
	// direct graph:    (SRC) ---------------> (RID)
	// fallback graph:    |--> (AD) --> (HID) -->|

	// dag starts with empty source node
	Node n_src;
	
	// create a DAG node of type RID
	// we make use of a special constructor in dagaddr.cpp which
	// takes in a pair (string type_str, string id_str) as argument and 
	// uses the Node::construct_from_strings() method: if type_str is not 
	// a built-in type, it will look for it the the user defined types set 
	// in /etc/xids, so this should work...
	Node n_rid(XID_TYPE_RID, rid_string);

	// 'fallback' nodes (AD and HID)
	Node n_ad(Node::XID_TYPE_AD, ad);
	Node n_hid(Node::XID_TYPE_HID, hid);

	// create the final DAG and a sockaddr_x * struct which can be used
	// in an XSocket API call
	Graph direct_dag = n_src * n_rid;
	Graph fallback_dag = n_src * n_ad * n_hid * n_rid;

	Graph full_dag = direct_dag + fallback_dag;

	// finally, fill the sockaddr_x * struct
	//rid_addr = (sockaddr_x *) malloc(sizeof(sockaddr_x));
	full_dag.fill_sockaddr(rid_addr);
	*rid_addr_len = sizeof(sockaddr_x);
}

void to_cid_addr(
	char * cid_string,
	sockaddr_x * cid_addr,
	socklen_t * cid_addr_len) 
{
	Node n_src;
	// assume cid_string is of the form "CID:0202..."
	Node n_cid(XID_TYPE_CID, cid_string);	

	Graph cid_dag = n_src * n_cid;

	// finally, get the sockaddr_x * struct
	//cid_addr = (sockaddr_x *) malloc(sizeof(sockaddr_x));
	cid_dag.fill_sockaddr(cid_addr);
	*cid_addr_len = sizeof(sockaddr_x);
}
