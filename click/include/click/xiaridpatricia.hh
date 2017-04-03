#ifndef CLICK_XIARIDPATRICIA_HH
#define CLICK_XIARIDPATRICIA_HH

#include <click/config.h>
#include <click/glue.hh>
#include <click/xid.hh>
#include <click/confparse.hh>
#include <click/straccum.hh>
#include <click/integers.hh>
#include <click/standard/xiaxidinfo.hh>
#include <click/args.hh>
#include <click/xiautil.hh>
#include <click/string.hh>
#include <click/glue.hh>

#include <clicknet/xia.h>
#include <click/xiaridutil.hh>

CLICK_DECLS

#define DEFAULT_KEY_BIT 0

// XXX: max size of an RID prefix string (the '+2' is for the '0x' of an
// hex number)
#define XIA_MAX_RID_STR	((2 * CLICK_XIA_XID_ID_LEN) + 2)

// XXX: modes for printing an RID PT
#define PRE_ORDER 	0x00
#define IN_ORDER 	0x01
#define POST_ORDER 	0x02

/**
 * \brief	represents a PATRICIA (PT) trie node and exposes several PT
 * 			operations (e.g. insertion, search, removal, etc.) adapted to
 * 			RID-like matching.
 *
 * XXX: to make this class usable by different Click elements, we make this a
 * class template: this explains the implementation of the member functions in
 * the header file.
 */
template <class T>
class XIARIDPatricia {

	public:

	/*
	 * ****************************************
	 * ENUMs & STUFF
	 * ****************************************
	 */

	enum CREATION_MODE {
		DEFAULT_NODE = 0x00,
		ROOT_NODE = 0x01
	};

	enum RID_MATCH_MODE {

		EXACT_MATCH = 	0x00,
		PREFIX_MATCH = 	0x01
	};

	/*
	 * ****************************************
	 * CONSTRUCTORS
	 * ****************************************
	 */

	/**
	 * \brief	default constructor
	 */
	XIARIDPatricia() {

		// XXX: default RID set to 0 (FIXME: type is also left at 0?)
		memset(&(this->rid), 0, sizeof(this->rid));
		this->key_bit = DEFAULT_KEY_BIT;
		this->left = this->right = NULL;
		this->data = NULL;
	}

	/**
	 * \brief	alternative constructor for an RID PT node
	 * 
	 * accepts a mode arg : if set to ROOT_NODE, it points left and right 
	 * pointers to the node being created. this should be used for the root 
	 * nodes in a new PT (e.g. used in XTRANSPORT::Xbind()).
	 * 
	 * \arg mode 	PT node creation mode: if set to ROOT_NODE, it points the 
	 * 				left and right pointers to the node being created.
	 */
	XIARIDPatricia(XIARIDPatricia<T>::CREATION_MODE mode) {

		memset(&(this->rid), 0, sizeof(this->rid));
		this->key_bit = DEFAULT_KEY_BIT;
		this->data = NULL;

		if (mode == XIARIDPatricia<T>::ROOT_NODE) {

			this->left = this->right = this;

		} else {

			this->left = this->right = NULL;
		}
	}

	/**
	 * \brief	alternative constructor for an RID PT node
	 * 
	 * accepts the <RID, data> pair to be associated with the PT node.
	 *
	 * \arg	rid			the RID of the new PT node
	 * \arg	data 		pointer to node's data
	 */
	XIARIDPatricia(
			const XID & rid,
			T * data) {

		this->rid = rid;
		this->data = data;
		this->key_bit = DEFAULT_KEY_BIT;
		this->left = this->right = NULL;
	}

	/*
	 * ****************************************
	 * 'GETTERS' TO PRIVATE MEMBERS
	 * ****************************************
	 */

	XID get_rid() { return this->rid; }
	int get_key_bit() { return this->key_bit; }
	T * get_data() { return this->data; }

	/*
	 * *********************************************************
	 * PUBLIC METHODS (see definitions and explanation below)
	 * *********************************************************
	 */

	// node insertion
	XIARIDPatricia<T> * insert(
			const XID & rid, 
			T * data);

	// node removal
	int remove(const XID & rid);

	// rid lookup : finds *ALL* nodes in a PT which match the rid being looked 
	// up
	int lookup(
			const XID & rid,
			int prev_key_bit,
			void (*callback)(void * obj_ptr, T * data),
			void * obj_ptr);

	// rid search : checks if rid exists in a PT
	XIARIDPatricia<T> * search(
		const XID & rid, 
		XIARIDPatricia<T>::RID_MATCH_MODE mode = XIARIDPatricia<T>::PREFIX_MATCH);

	// rid update : update the data field in a node in the PT
	XIARIDPatricia<T> * update(const XID & rid, T * data);

	// node count
	int count();

	// print the PT
	String print(
			uint8_t mode,
			unsigned int data_str_size,
			String (*print_data)(T * data));


	private:

	/*
	 * ****************************************
	 * PRIVATE ATTRIBUTES
	 * ****************************************
	 */

	// FIXME: we assume PT nodes represent RIDs w/ some
	// associated data. nevertheless, it
	// would be great to have this as a full-fledged class template, i.e
	// having the key of a generic type instead of XID.
	XID rid;

	// bit to check (aka `key bit')
	int key_bit;

	// pointers to left and right sub-tries
	XIARIDPatricia<T> * left;
	XIARIDPatricia<T> * right;

	// XXX: each PT node holds a pointer to a piece of data of any type
	// (e.g. a XIARouteData object, an integer representing a socket port
	// number, etc.)
	T * data;

	/*
	 * ****************************************
	 * PRIVATE MEMBER FUNCTIONS
	 * ****************************************
	 */

	uint8_t _is_bit_set(int i, const XID & rid);
	bool _is_leaf(XIARIDPatricia<T> * node);

	XIARIDPatricia<T> * _insert_recursive(
			XIARIDPatricia<T> * curr_node,
			XIARIDPatricia<T> * new_node,
			int new_key_bit,
			XIARIDPatricia<T> * parent_node);

	int _count_recursive(
			XIARIDPatricia<T> * node,
			int prev_key_bit);

	void _print_recursive(
			char ** pt_str,
			unsigned int node_str_size,
			XIARIDPatricia<T> * node,
			uint8_t mode,
			String (*print_data)(T * data));
};

/*
 * ****************************************
 * PUBLIC METHODS
 * ****************************************
 */

/*
 * ****************************************
 * INSERT()
 * ****************************************
 */

/**
 * \brief 	inserts a new node (w/ key set to a provided RID) into the sub-PT
 * 			`rooted' at the calling node.
 *
 * \arg	rid	RID of the new node to insert
 *
 * \return	a pointer to the new node, if the insertion is successful. NULL
 * 			otherwise.
 */
template<class T>
XIARIDPatricia<T> * XIARIDPatricia<T>::insert(
		const XID & rid,
		T * data) {

	// XXX: the insertion of a new node in a PT works like this:

	//	1) search for the closest matching 'leaf' node: this will help
	//		determine the insertion point for the new node

	//	2) find the first bit index (from 0 to 159) at which the new node
	//		differs from the 'insertion point'. this will be the 'key bit' of
	//		the new node.

	//	3) a recursive insertion step follows, in which the sub-PT is
	//		traversed from `top-to-bottom', always comparing the `key bit'
	//		found in the previous step with that of the nodes found along
	//		the search path. the objective is to find 1 of 2 situations:
	//
	//		3.1) finding 2 nodes A and B in between each the new node should be
	//			inserted
	//		3.2) finding a leaf node - pointing 'up' the PT - after which the
	//			new node will be inserted

	// find the closest matching leaf node. note that we're using the calling 
	// node (i.e. 'this') as a starting point for insertion.
	XIARIDPatricia<T> * closest = this;
	int i = 0;

	click_chatter("XIARIDPatricia::insert() : inserting new node (rid = %s) at PT node w/ <%s, %d>\n", 
		rid.unparse().c_str(), 
		closest->rid.unparse().c_str(), 
		closest->key_bit);

	do {

		i = closest->key_bit;

		closest = (_is_bit_set(closest->key_bit, rid) ? closest->right : closest->left);

	} while (i < closest->key_bit);

	// if such a node already exists, return NULL
	if (closest->rid == rid)
		return NULL;

	// find the first bit that differs between the node to insert and the
	// closest matching leaf node: this tells which 'key bit' to set in the
	// PT node which is about to become a parent node

	for (
			i = 1;
			i < ((8 * CLICK_XIA_XID_ID_LEN) - 1)
					&& (_is_bit_set(i, rid) == _is_bit_set(i, closest->rid));
			i++);

	click_chatter("XIARIDPatricia::insert() : first bit that differs : %d\n", i);

	// recursive step: this is where the actual insertion happens. we start
	// inserting the node down the sub-PT, starting on the 'root' of the sub-PT
	XIARIDPatricia<T> * new_node = new XIARIDPatricia<T>(rid, data);

	if (_is_bit_set(this->key_bit, rid)) {

		this->right = _insert_recursive(this->right, new_node, i, this);

	} else {

		this->left = _insert_recursive(this->left, new_node, i, this);
	}

	return new_node;
}

/*
 * ****************************************
 * REMOVE()
 * ****************************************
 */
/**
 * \brief removes a PT node on the calling node sub-PT, given an RID
 *
 * this removal method roughly follows the steps below:
 * 	-# find node N which contains 'key'
 *	-# copy N's parent (P) key (and data) to N
 *	-# do some 're-wiring' so that trie connections go from and go to
 *		N's grand parent points to N, bypassing P
 *	-# delete P
 *
 * \arg	rid	the RID of the node to be deleted
 *
 * \return	an integer: 1 if removal is successful, 0 if node not found, -1 if
 * 			the removal fails.
 */
template<class T>
int XIARIDPatricia<T>::remove(const XID & rid) {

	// XXX: try to explain how the node removal works in the numbered
	// comments
	//
	// 	1) search for the node with RID, similarly to the method used in
	//		search(), but keeping track of parent (p) and grandparent (gp)
	//		nodes
	XIARIDPatricia<T> * target, *p, *gp;

	int i = 0;
	target = p = gp = this;

	do {

		i = target->key_bit;

		gp = p;
		p = target;
		target = (_is_bit_set(target->key_bit, rid) ? target->right : target->left);

	} while (i < target->key_bit);

	// 1.1) if the found node is the head of the sub-PT, abort
	if (target == this)
		return -1;

	// 1.2) if the found node doesn't hold rid, abort (with a diff. return code)
	if (target->rid != rid)
		return 0;

	//	2) make all necessary re-wiring's so that p is completely bypassed and
	// 		safe to remove, i.e. all the nodes pointing to p (gp and nodes
	//		'from below') are re-wired to target

	// 	2.1) if p is a `leaf' node - i.e. if both its left and right pointers
	// point 'up' the PT - then all we need to do is bypass the gp's link to
	// p's outgoing link which does NOT point to target

	// FIXME: this condition is used by Radu Gruian in Code Project (and not in
	// Matt Smart's) to accommodate the following cases:
	//	1) both outgoing links in p are self-links: make the gp point
	// 		to itself too;
	//	2) only 1 link is a self-link, which includes the cases
	//		{ {self-link, up-link}; {up-link, self-link};
	//		{up-link, up-link} }

	// case 1, is it possible? By definition, NO: each node has only 1 key and
	// looks at 1 bit of the key only, so two self-links to p could only happen
	// if p->key_bit bit in p->rid was 0 and 1 at the same time.

	// case 2 : the first 2 sub-cases would be well handled by the general
	// steps after 2.2 (used by Matt Smart). the 3rd sub-case is also
	// impossible by definition: if p is a 'leaf', then it must have a
	// self-link at one of its branches, otherwise a search for p->key would
	// never be successful, despite its existence in the PT.

	// XXX: for the reasons summarized above, the approach from Radu Gruian
	// will be discarded: the final solution is much closer to that of
	// Matt Smart's

//	if (_is_leaf(p)) {
//
//		if (gp != p) {
//
//			// find which node should gp point to
//			XIARIDPatricia<T> * next =
//					(((p->left == p->right) && (p->left == p)) ? gp : ((p->left == p) ? p->right : p->left));
//
//			// find which branch would be chosen if we had to lookup rid at
//			// gp's
//			if (_is_bit_set(gp->key_bit, rid)) {
//
//				gp->right = next;
//
//			} else {
//
//				gp->left = next;
//			}
//		}
//
//	} else {

	// 2.2) starting from p, get to the node which is pointing to p 'from
	// below', pp.
	XIARIDPatricia<T> *pp, *pp_temp;
	pp = pp_temp = p;

	// 2.2.1) we find pp by searching for p's RID, starting from p
	do {

		i = pp_temp->key_bit;

		pp = pp_temp;
		pp_temp = (_is_bit_set(pp_temp->key_bit, p->rid) ? pp_temp->right : pp_temp->left);

	} while (i < pp_temp->key_bit);

	// 2.2.2) something really wrong happened: a 'from below' pointer to
	// p doesn't exist
	if (pp_temp->rid != p->rid)
		return -1;

	// 2.3) re-wire pp so that it points towards target (instead of p),
	// since target now replaces p (same key/data as p, different key_bit)
	if (_is_bit_set(pp->key_bit, p->rid)) {

		pp->right = target;

	} else {

		pp->left = target;
	}

	// 2.4) re-wire gp so that it bypasses p

	// 2.4.1) find on which branch of p is its other `sibling' (i.e.
	// other than target): check which branch would search() take if
	// looking for rid, then use the other branch
	XIARIDPatricia<T> * sibling = (_is_bit_set(p->key_bit, rid) ? p->left : p->right);

	// 2.4.2) find which branch would be chosen if we had to lookup
	// rid at the gp node, then bypass the right branch to sibling
	if (_is_bit_set(gp->key_bit, rid)) {

		gp->right = sibling;

	} else {

		gp->left = sibling;
	}
//	}

	//	3) copy p's key and data to target's
	// XXX: it only makes sense to do it when N and P are not the same
	// physical node. this often happens with PT leaf nodes that have
	// self-links.
	if (target != p) {

		target->rid = p->rid;

		// FIXME: this requires attention: should we assume a '=' operator,
		// some a copy constructor or memcopy() ourselves here? i'll assume
		// the existence of a copy constructor...
		delete target->data;
		target->data = new T(p->data);
	}

	// 4) finally, delete p
	delete p;

	return 1;
}

/*
 * ****************************************
 * LOOKUP()
 * ****************************************
 */

/**
 * \brief 	traverses the whole sub-PT, searching for ALL positive RID
 * 			matches against some given RID. invokes a callback function
 * 			for every positive match (TP or FP).
 *
 * FIXME: we use recursion to traverse the PT. FPs can produce
 * multiple positive matches (TP or FP), and an action is required for
 * each of such matches.
 *
 * instead of having lookup() return a variable
 * number of matches (e.g. via a variable sized array passed as a
 * 'value-result' arg), we use a callback-based solution:
 *
 * 	1) an object (say OBJ) calling lookup() must also pass a pointer to a
 * 		customized callback function, which acts on the data field of a
 * 		PT node;
 * 	2) since callback() may also need to act on information known only
 * 		to OBJ, a void pointer to OBJ is also passed in lookup() and
 * 		callback(): this way, callback() - which needs to be a static
 * 		member function - is able to call non-static members of OBJ.
 *
 * \arg rid				the RID being looked up (e.g. an RID in a request)
 * \arg prev_key_bit	the `key bit' of the preceding node (for detecting
 * 						`up links', ending the lookup procedure)
 * \arg callback		pointer to a callback function, which acts on the
 * 						node's data, to be invoked at every node
 * 						matching rid.
 * \arg obj_ptr			pointer to the object which should call callback().
 *
 * \return	an integer, the number of positive matches (TPs and FPs
 * 			included) detected in the lookup.
 */
template<class T>
int XIARIDPatricia<T>::lookup(
		const XID & rid,
		int prev_key_bit,
		void (*callback)(void * obj_ptr, T * data),
		void * obj_ptr) {

	click_chatter("XIARIDPatricia<T>::lookup()) : [ENTER] (%d vs. %d)\n", this->key_bit, prev_key_bit);

	int pos_matches = 0;

	// stop the lookup if the value to the key bit decreases: this means
	// we're going up the trie
	if (this->key_bit <= prev_key_bit) {

		click_chatter("XIARIDPatricia<T>::lookup()) : [RETURN] 0\n");
		return pos_matches;
	}

	click_chatter("XIARIDPatricia<T>::lookup()) : matching...\n");

	// XXX: when looking up an RID PT (accounting for FPs), we always
	// follow the left branch of the PT, and selectively follow the
	// right branch.

	// "ok... why?", you may ask...

	// say that F is the RID of the PT which we're currently
	// comparing against the request's RID, R. consider also that mask(R)
	// represents the key_bit leftmost bits of R.

	// by definition, mask(F->left) will always have a '0' bit at position
	// F->key_bit: the RID matching test
	// ((match(R) & match(F->left)) == match(F->left)) will always be true.
	// therefore we should always follow the left sub-PT.

	// on the other hand, if mask(F) has a '1' bit at position F->key_bit,
	// then (by definition) all 'Fs' under F->right will have a '1' bit at
	// position F->key_bit. therefore, if the test ((match(R) & match(F))
	// == match(F)) is false (which can only happen if F has a '1' at
	// F->key_bit), then we can bypass the right sub-PT.
	if (rid_match_mask(rid, this->rid, this->key_bit)) {

		click_chatter("XIARIDPatricia<T>::lookup()) : %s partially matches w/ %s (mask %d)\n",
			rid.unparse().c_str(), this->rid.unparse().c_str(), this->key_bit);

		// FIXME: avoid matches with the default route (or root
		// node) by checking node->key_bit > 0.
		if (rid_match(rid, this->rid) && (this->key_bit > 0)) {

			click_chatter("XIARIDPatricia<T>::lookup()) : %s matches w/ %s (mask %d)\n",
				rid.unparse().c_str(), this->rid.unparse().c_str(), this->key_bit);

			// XXX: a positive match (either TP or FP) has been found: call
			// the callback function on the node's data
			pos_matches++;

			click_chatter("XIARIDPatricia<T>::lookup()) : it's callback time!\n");
			(*callback)(obj_ptr, this->data);
		}

		// keep looking up down the right sub-PT
		pos_matches += this->right->lookup(rid, this->key_bit, callback, obj_ptr);
	}

	// regardless of matches, allways follow the left sub-PT
	pos_matches += this->left->lookup(rid, this->key_bit, callback, obj_ptr);

	click_chatter("XIARIDPatricia<T>::lookup()) : [RETURN] 1\n");
	return pos_matches;
}

/*
 * ****************************************
 * SEARCH()
 * ****************************************
 */

/**
 * \brief	exact-match-like search for a given RID
 *
 * \arg		rid	RID to search for
 *
 * \result	a pointer to the RID PT node (if found), NULL otherwise.
 */

// RID:000200044008a010864400545705482021400c80
// RID:0000000440000000044000400004400000000400

// Click: XIARIDPatricia<T>::search() : key_bit = 0
// Click: XIARIDPatricia::_is_bit_set() : checking bit at position 0 (19), i.e. (rid[128] AND 80) = 1ED5550

// RID:0011010540400001454001404045444041455540
// RID:0000000440000000044000400004400000000400

// Click: XIARIDPatricia<T>::search() : key_bit = 0
// Click: XIARIDPatricia::_is_bit_set() : checking bit at position 0 (19), i.e. (rid[128] AND 00) = F9C740

template<class T>
XIARIDPatricia<T> * XIARIDPatricia<T>::search(
	const XID & rid,
	XIARIDPatricia<T>::RID_MATCH_MODE mode) {

	XIARIDPatricia<T> * curr = this;

	// saves previous key_bit, so that we can test if we're going down or up
	// the PT
	int i = 0;

	do {

		// XXX: Q: how will curr->key_bit be ever less than or equal to i?

		// A: this is the typical stopping condition for a search in
		// in PATRICIA tries. since there are no explicit NULL links, we
		// verify if any of the links 'points up' the trie (if you
		// inspect _insert_recursive(), you'll see that sometimes
		// curr->right or curr->left point to the head of the tree, parent
		// node or itself). this can be done because (by definition) the
		// `key bit' in the nodes increase as one traverses down the trie.
		click_chatter("XIARIDPatricia<T>::search() : key_bit = %d\n", curr->key_bit);

		i = curr->key_bit;
		curr = (_is_bit_set(curr->key_bit, rid) ? curr->right : curr->left);

	} while (i < curr->key_bit);

	// despite being the best match: is this the PT that we really want?

	if (mode == XIARIDPatricia<T>::EXACT_MATCH)
		return ((curr->rid == rid) ? curr : NULL);
	else
		return ((rid_match(rid, curr->rid)) ? curr : NULL);
}

/*
 * ****************************************
 * UPDATE()
 * ****************************************
 */
template<class T>
XIARIDPatricia<T> * XIARIDPatricia<T>::update(
	const XID & rid,
	T * data) {

	// find the PATRICIA trie node w/ rid
	XIARIDPatricia<T> * to_update = search(rid);
	// update its data value (if existent)
	if (to_update) to_update->data = data;

	return to_update;
}

/*
 * ****************************************
 * COUNT()
 * ****************************************
 */

/**
 * \brief	counts nr. of nodes in an RID sub-PT
 *
 * \return	an integer, number of nodes in the calling sub-PT
 */
template<class T>
int XIARIDPatricia<T>::count() { return _count_recursive(this, -1); }

/*
 * ****************************************
 * PRINT()
 * ****************************************
 */

/**
 * \brief 	generates a String with the entire RID PT, one node per line,
 * 			using a custom print function for the node's data (passed as a
 * 			function pointer)
 *
 * \arg mode			PTs can be traversed - and therefore printed - in
 * 						PRE_ORDER, IN_ORDER or POST_ORDER mode
 * \arg data_str_size	max possible size of a string representation of the
 * 						PT node's data
 * \arg print_data		function pointer to generate a custom string
 * 						representation of the PT node's data
 *
 * \return	a String holding the entire RID PT, one node per line
 */
template<class T>
String XIARIDPatricia<T>::print(
		uint8_t mode,
		unsigned int data_str_size,
		String (*print_data)(T * data)) {

	click_chatter("XIARIDPatricia<T>::print() : [ENTER] printing PATRICIA trie w/ HW = %d\n", rid_calc_weight(this->rid));

	// XXX: calculate total size (in byte) to be occupied by the output string

	// size of a node entry line, which includes:
	//	1) RID prefix: 2 * CLICK_XIA_XID_ID_LEN + 2 chars (+2 for '0x' of hex
	//		number
	//	2) data representation: passed as argument
	//	3) 11 additional chars for formatting purposes + '\n' + \0' chars
	int node_str_size = (XIA_MAX_RID_STR + data_str_size + 11 + 1 + 1);

	// total size for the PT string multiplies node_str_size by the
	// nr. of entries in the PT
	int pt_str_size = this->count() * (node_str_size);
	char * pt_str = (char *) calloc(pt_str_size, sizeof(char));

	// XXX: initiate the recursive printing call
	_print_recursive(&pt_str, node_str_size, this, mode, print_data);

	click_chatter("XIARIDPatricia<T>::print() : PATRICIA trie :\n%s\n", pt_str);

	String s = String(pt_str);
	free(pt_str);

	return s;
}

/*
 * ****************************************
 * PRIVATE MEMBER FUNCTIONS
 * ****************************************
 */

/**
 * \brief	returns whether or not the i-th bit is set in an RID (i between 0
 * 			and 159, starting from the rightmost bit).
 */
template<class T>
uint8_t XIARIDPatricia<T>::_is_bit_set(int i, const XID & rid) {

	// let's look at the logic from the return expression in parts:

	// A) [CLICK_XIA_XID_ID_LEN - (i / 8) - 1]
	// B) (1 << (i % 8))

	// A) [CLICK_XIA_XID_ID_LEN - (i / 8) - 1]: find the byte in
	//    the RID where the i-th bit should be located (from 0 to
	//    (CLICK_XIA_XID_ID_LEN - 1)). 
	//
	//    the first bit (i = 0) is at the largest byte index (e.g. bit 0 is  
	//    at rid[CLICK_XIA_XID_ID_LEN - 1]), the last bit 
	//    (i = 159) is at rid[0].
	//
	// B) (1 << (i % 8)): the position of bit i WITHIN one byte. the idea is 
	//    to create a bitmask which isolates the i-th bit in the rid byte found 
	//    in step A. 
	//
	// e.g. say that i = 13. in step A we find that the 13-th bit is in 
	// rid[18]. in step B we create a 1-byte bitmask equal to 0x20, since 
	// the 13-th bit is also the bit at index 5 in the rid[18] byte.
	//
	// we finally AND (&) the results of steps A and B	to find out if the 
	// 13-th bit is set (or not).

	click_chatter("XIARIDPatricia::_is_bit_set() : checking bit at position %d, ((rid[%d] = %02X) AND %02X) = %02X\n", 
		i,
		CLICK_XIA_XID_ID_LEN - (i / 8) - 1,
		rid.xid().id[CLICK_XIA_XID_ID_LEN - (i / 8) - 1],
		(1 << (i % 8)),
		rid.xid().id[CLICK_XIA_XID_ID_LEN - (i / 8) - 1] & (1 << (i % 8)));

	return rid.xid().id[CLICK_XIA_XID_ID_LEN - (i / 8) - 1] & (1 << (i % 8));
}

/**
 * \brief	private function used for recursively inserting a new node in a
 * 			RID PT
 *
 * \arg curr_node	node in the PT currently being examined
 * \arg new_node	new node to insert
 * \arg new_key_bit	decision bit for the new node to insert
 * \arg parent_node	parent of the current node
 *
 *	return	a pointer to a PT node, either the new node (when the recursive
 *			insertion reaches the end) or the expected next node (either on the
 *			left or right sub-PT) for other cases
 */
template<class T>
XIARIDPatricia<T> * XIARIDPatricia<T>::_insert_recursive(
		XIARIDPatricia<T> * curr_node,
		XIARIDPatricia<T> * new_node,
		int new_key_bit,
		XIARIDPatricia<T> * parent_node) {

	// XXX: stop the recursion when you stumble upon 1 of 2 situations (the 2
	// conditions in the if statement):
	//	1) the new node has a `key bit' which sits between 2 nodes A and B,
	//		already held in the PT: we stop before following any further
	//		link leaving B, and so, when the new_node is returned, it is
	//		inserted in between A and B

	//	2) a leaf node is reached, which points 'up' the PT (either to itself
	//		or to a node higher up in the PT): in this case, we want to insert
	//		the new node to the right or left of the curr_node, which happens
	//		because we stop the recursive process after following a link 'up'
	//		from the leaf node
	//
	// in both cases, the only thing we do within the if block is to fill
	// in the info of the new node
	if ((curr_node->key_bit >= new_key_bit)
			|| (curr_node->key_bit <= parent_node->key_bit)) {

		new_node->key_bit = new_key_bit;

		new_node->left = _is_bit_set(new_key_bit, new_node->rid) ? curr_node : new_node;
		new_node->right = _is_bit_set(new_key_bit, new_node->rid) ? new_node : curr_node;

		return new_node;
	}

	// XXX: if _insert_recursive() returns:
	//	1) curr_node: the PT config stays the same
	// 	2) new_node: either the left or right link of curr_node is changed to
	//		new_node
	if (_is_bit_set(curr_node->key_bit, new_node->rid)) {

		curr_node->right = _insert_recursive(curr_node->right, new_node, new_key_bit, curr_node);

	} else {

		curr_node->left = _insert_recursive(curr_node->left, new_node, new_key_bit, curr_node);
	}

	return curr_node;
}

/**
 * \brief	verifies if an RID PT node is a PT 'leaf' or not
 *
 * A node is considered a PT 'leaf' if both its left and right pointers point
 * 'up' the PT.
 *
 * \return	TRUE if the node is a PT 'leaf', FALSE if otherwise.
 */
template<class T>
bool XIARIDPatricia<T>::_is_leaf(XIARIDPatricia<T> * node) {

	// XXX: remember, in PTs, child nodes further down the trie have strictly
	// increasing key_bit values, so if we detect that a child node has
	// at least an equal key_bit value, then we know we have reached a PT 'leaf'
	if (node->right->key_bit < node->key_bit
			|| node->left->key_bit < node->key_bit) {

		return false;
	}

	return true;
}

/**
 * \brief	counts nr. of nodes in an RID sub-PT rooted at node
 * 			
 *
 * \arg		node			the node at which the sub-PT to be counted
 * 							is rooted
 * \arg		prev_key_bit	the key_bit of the node used in a previous
 * 							recursive call (used for stop condition).
 * 							when starting from the PT's root, -1 should
 * 							be used.
 *
 * \return	an integer, number of nodes in the sub-PT rooted at node
 */
template<class T>
int XIARIDPatricia<T>::_count_recursive(
		XIARIDPatricia<T> * node,
		int prev_key_bit) {

	if (node->key_bit <= prev_key_bit)
		return 0;

	int count = 1;

	count += _count_recursive(node->left,  node->key_bit);
	count += _count_recursive(node->right, node->key_bit);

	return count;
}

/**
 *
 */
template<class T>
void XIARIDPatricia<T>::_print_recursive(
		char ** pt_str,
		unsigned int node_str_size,
		XIARIDPatricia<T> * node,
		uint8_t mode,
		String (*print_data)(T * data)) {

	// FIXME: we ignore PT traversing modes for now
//	if (mode == PRE_ORDER) {

	// FIXME: all these calloc() + recursive call combinations can't be good
	// for a router...
	char * node_str = (char *) calloc(node_str_size, sizeof(char));
	char * prefix_bytes = (char *) calloc(XIA_MAX_RID_STR, sizeof(char));
	
	// 1) print the fwd entry info on node_str, with the following format
	//	[<RID_PREFIX> (<KEY_BIT>)], <DATA>



	// XXX: some details
	//	1) RID_PREFIX consists in (node->key_bit + 1) leftmost bits of the RID
	//		saved in the node
	sprintf(node_str, "[%s (%d)], %s\n",
			rid_extract_prefix_bytes(&prefix_bytes, node->rid, node->key_bit + 1),
			node->key_bit,
			((*print_data) (node->data)).c_str());

	// 2) append the node_str to the given PT global str
	strncat(*pt_str, node_str, node_str_size);

	free(prefix_bytes);
	free(node_str);

	// XXX: recursively call _print_recursive() if the PT continues downwards
	if (node->left->key_bit > node->key_bit)
		node->left->_print_recursive(pt_str, node_str_size, node->left, mode, print_data);

	if (node->right->key_bit > node->key_bit)
		node->right->_print_recursive(pt_str, node_str_size, node->right, mode, print_data);
//	}
}

CLICK_ENDDECLS
#endif
