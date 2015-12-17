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

void bloomifyPrefix(
		struct bloom * bloomFilter,
		char * _prefix)
{

	// transform the URL-like prefix into a Bloom Filter (BF) of size
	// m = 160 bit

	// the BF itself
	bloom_init(bloomFilter);

	// by default, we consider all possible prefix lengths
	// TODO: implement a non-default behavior (e.g. argument specifying the
	// prefix lengths to encode)

	// do this in order not to destroy _prefix in strtok()
	// TODO: can we do better than this?
	char * prefix = (char *) calloc(PREFIX_MAX_LENGTH, sizeof(char));
	strncpy(prefix, _prefix, strlen(_prefix));

	// to sequentially build the subprefixes
	char * subPrefix = (char *) calloc(PREFIX_MAX_LENGTH, sizeof(char));

	// holds the end index of each subprefix in prefix, according to subprefix
	// size
	int * subPrefixIndex = (int *) calloc(PREFIX_MAX_COUNT, sizeof(int));
	int subPrefixCount = 0;

	size_t subPrefixLen = 0;
	size_t prefixLen = strlen(prefix);

	// extract the first subprefix (i.e. from start of prefix till the first
	// occurrence of '/')
	char * token;
	token = strtok(prefix, PREFIX_DELIM);

	int bfCollisions = 0;

	while (token != NULL
			&& !(subPrefixCount > PREFIX_MAX_COUNT)) {

		// concatenate the next token between '/' to the current subprefix
		subPrefix = strncat(
				subPrefix,
				(const char *) token,
				(PREFIX_MAX_LENGTH - strlen(subPrefix) - 1));

		subPrefixLen = strlen(subPrefix);

		// TODO: shall we include '/' for the BF encoding part?
		// check if this is the final token
		if (subPrefixLen < prefixLen) {

			subPrefix = strncat(
					subPrefix,
					PREFIX_DELIM,
					(PREFIX_MAX_LENGTH - subPrefixLen - 1));

			subPrefixLen++;
		}

		// update the index in prefix at which the current subprefix ends
		subPrefixIndex[subPrefixCount++] = subPrefixLen;

		// add the current subprefix to the BF
		//printf("adding prefix %s, size %d\n", subPrefix, subPrefixLen);

		if(bloom_add(bloomFilter, subPrefix, subPrefixLen)) { bfCollisions++; }

		// capture the new token
		token = strtok(NULL, PREFIX_DELIM);
	}

	//bloom_print(bloomFilter);
}

char * toRIDString(unsigned char * xid) {

	int i = 0, ridDAGPrefixLen = strlen(RID_STR_PREFIX);

	char * ridString = (char *) calloc(
									RID_STR_SIZE,
									sizeof(char));

	// add the initial prefix to ridString, i.e. "RID:"
	strncpy(ridString, RID_STR_PREFIX, ridDAGPrefixLen);

	// add the actual XID to ridString.

	// XXX: note we build the ridString in the following format:
	//	"RID:" + xid[0] + xid[1] + ... + xid[XID_SIZE - 1]
	// is this correct? shouldn't it be:
	//	"RID:" + xid[XID_SIZE - 1] + xid[XID_SIZE - 2] + ... + xid[0]
	// guess that, if an analogous toRIDByte() decodes the string in a similar
	// way (i.e. considering xid[0] right after "RID:") then there's no
	// problem...
	for (i = 0; i < XID_SIZE; i++) {

		// add xid[i], formatted to %02x every 2 byte, with an offset of
		// ridDAGPrefixLen, so that xid[i] start being written after the
		// "RID:" part
		snprintf(
				ridString + (i * 2) + ridDAGPrefixLen,
				RID_STR_SIZE,
				"%02x",
				(int) (xid[i]));
	}

	return ridString;
}
