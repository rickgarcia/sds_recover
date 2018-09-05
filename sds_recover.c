#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>

#define SDS_HDR_SIZE 16

#define SDS_INT		1
#define SDS_CHAR	6
#define SDS_STRING	7
#define SDS_FLOAT	16
#define SDS_STRUCT	21
#define SDS_VOID	22
#define SDS_STRUCT_LIST	24
#define SDS_BASE64	27
#define SDS_TYPEMAX     28

// the header identifier is at most this
#define HEADER_MAX    8

//#define DEBUG 

/* sds_record
 * - malloc'd with the expected extra data size tack'd on the end.
 *  - offset      : file offset marking the start of the record
 *  - headera     : header id tag
 *  - h_spacer    : undocumented field
 *  - type        : data type constant
 *  - size        : expected data size
 *  - data_buffer : actual data
 */
struct sds_record {
	unsigned int offset;
	int header;
	int h_spacer;
	int type;
	int size;
	char data_buffer[];
};


/* validate_h_spacer
 * the h_spacer field is not documented, so we're left to guess what the
 * significance of the values are.  Grab values attached to valid data and 
 * add them to the list as we id them.
 *
 * known header spacer values */
unsigned int h_spacers[] = { 0x0, 0x83ff, 0x7ff2, 0x2abb };
int validate_h_spacer(unsigned int hs_value) 
{
	int idx = sizeof(h_spacers) / sizeof(unsigned int);
	while (--idx >= 0) {
		if (h_spacers[idx] == hs_value) {
			return 1;
		}
	}
	return 0;
}


/* scan_sds_datum
 * scans a single sds item and returns length for validity and 0 for error 
 *  - _ptr : is the buffer head 
 *  - sz   : is the max size to parse before erroring out 
 *
 *
 * SDS datum fields:
 *	name		null-terminated string
 *	data_type	null-terminated string; atoi value is integer 0-28
 *	data_length	null-terminated string; atoi value not limited
 *	data...		format depends on type; most integers/floats are 
 *			given in standard string format
 * 
 * The data field is not always null-terminated.  VOID, STRUCT, and LIST types
 * have sizes specified in the header.  Checking for null-terminated data 
 * fields is done by strlen; checking is reasonably strict, but not perfect 
 * 
 */
//#define DEBUG_sds_scan
int scan_sds_datum(unsigned char *_ptr, int sz) 
{
	unsigned char *pname = _ptr;
	unsigned char *ptype = NULL, *plen = NULL, *pdata = NULL;
	int name_length = -1;
	int type_length = -1,  len_length = -1, data_length = -1; 
	int data_type = -1;

	/* The above fields should be null-terminated; keep the ptrs in a list
	 * for easier reference while we loop thru the buffer; we've already
	 * got the start of the name field, so we start from the type */
	unsigned char **list[3] = { &ptype, &plen, &pdata };
	int _list_idx = 0;
	
	/* loop thru the data record and fill out the fields */
	while (sz-- > 0) {

		/* data type field checking */
		if ((ptype != NULL) && (data_type == -1)) {
			/* conversions ... add 1 to include the \0 */
			type_length = strlen((char*)ptype) + 1;
			data_type = atoi((char*)ptype);

			/* all data types are below TYPEMAX */
			if (data_type >= SDS_TYPEMAX) {
				return 0;
			}

			/* specific type handling...*/
			switch (data_type) {
				case SDS_INT : 
				case SDS_CHAR :
				case SDS_STRING :
				case SDS_FLOAT :
				case SDS_STRUCT :
				case SDS_VOID :
				case SDS_STRUCT_LIST :
				case SDS_BASE64 :
					break;
				default : 
#ifdef DEBUG_sds_scan
					printf("unknown type %d\n", data_type);
					exit(1);
#endif
					return 0;
			};
			/* since we have the second field, we can check the
			 * value of the name field - add 1 to length for \0;
			 * check that the string length does not equal 0, and 
			 * is equal to the difference in the name ptr and the 
			 * current pointer position 
                         */
			name_length = strlen((char*)pname) + 1;
			if (
				(name_length == 0) ||
				(name_length != 
					((unsigned long)(_ptr - pname)))) {
				return 0;
			}
		}

		/* data length field checking - there's a limited amount
		 * we can do here without implementing different types of 
		 * checking for each data type - that can be left for 
		 * later if needed.  For now, just do basic sanity checking */
		if ((plen != NULL) && (data_length == -1)) {
			len_length = strlen((char*)plen) + 1;
			data_length = atoi((char*)plen);

			if ((data_length == 0) || (data_length >= sz)) {
				return 0;
			}
			/* the data_length should limit the scanning left */
			sz = data_length + 1;
		}

#ifdef DEBUG_sds_scan
		printf("sz : %d :0x%08x : (%c) : 0x%02x (%d)\n",
			sz, _ptr, *_ptr, *_ptr, _list_idx); 
#endif
                // might have a struct filled out
		if ((int)*_ptr == 0) {
#ifdef DEBUG_sds_scan
			printf("Zero status : [ 0x%08x : 0x%08x : 0x%08x ]\n",
				list[0], list[1], list[2]);
#endif
			unsigned char **tmp = list[_list_idx++];
			*tmp = (unsigned char* )(_ptr + 1);
			// break if we've got all our fields 
			if (_list_idx >= 3) {
				break;
			}
		}
		_ptr++;
	}
#ifdef DEBUG_sds_scan
	printf("0x%08x\n0x%08x\n0x%08x\n0x%08x\n",
		pname, ptype, plen, pdata);
	printf("\tName : %s\t(%d)\n", pname, name_length);
	printf("\tType : %s\t(%d)\n", ptype, type_length);
	printf("\tlen  : %s\t(%d)\n", plen, len_length);

	if ((data_type != SDS_STRUCT) && (data_type != SDS_VOID)) {
		printf("\tData : '%s'\t(%d)\n", pdata, data_length);
	}
#endif

	/* not all data fields are null deliminated, and some fields can 
	 * contain the \0 character as part of the data - ignore these, but
	 * perform string length verification on other types */
	if (
		(data_type == SDS_INT) || 
		(data_type == SDS_STRING) ||
		(data_type == SDS_FLOAT)) 
	{
		int _dstr_length = strlen((char*)pdata) + 1;
#ifdef DEBUG_sds_scan
		printf("strlen: %d  | speclen: %d\n", 
			_dstr_length, data_length);
#endif
		if (_dstr_length != data_length) {
#ifdef DEBUG_sds_scan
			printf("length mismatch\n");
#endif
			return 0;
		}
	}

	/* return the total length of this sds datum */
	return (name_length + type_length + len_length + data_length);
}


/* validate_sds_data
 * - run thru the payload data and verify that it's internally consistent
 * - on success, returns
 * - on error, returns the location of the error in the record
 */
int validate_sds_data(struct sds_record *sds) 
{
	unsigned char *data = (unsigned char*)(sds->data_buffer);
	int size = sds->size;

	while (size > 0 ) {
		/* scan returns good bytes on success, and 0 on failure or 
		 * end of record - if there are any bytes left when 0 is
		 * returned, then there was a failure - return the failure
		 * location */
		int valid = scan_sds_datum(data, size);
		if (valid == 0) { break; }
		data += valid; // push the data ptr ahead
		size -= valid; // .. and decrease the bytes left to check
	}
	// return unparsed data size if any
	return size;
}


/* print_record
 * - buffer : head of sds record contained in buffer
 * - sz     : maximum size to print
 *
 * This function calls itself to handle encapsulated structure types
 */
void print_record(char *buffer, int sz)
{
	static int indent = 0;
	int ct = 0;

	do {
		char *name_buf = NULL;
		char *type_buf = NULL;
		char *length_buf = NULL;
		void *pl_data = NULL;
		int len;

		//TODO: implement strict length checking for internal data
		name_buf = &(buffer[ct]);
		len = strlen(name_buf);
		ct += len + 1;

		type_buf = &(buffer[ct]);
		len = strlen(type_buf);
		ct += len + 1;

		length_buf = &(buffer[ct]);
		len = strlen(length_buf);
		ct += len + 1;

		pl_data = &(buffer[ct]);
	
		//TODO: would be nice to have a validation on these returns	
		int _type = atoi(type_buf);
		int _length = atoi(length_buf);

		// prettification
		int _indent = indent;
		while (_indent-- >= 0) { 
			fprintf(stdout, "\t"); 
		}

		if (_type == SDS_VOID) {
			fprintf(stdout, "%-16s : (%02d) : %03d\t'%x'\n", 
				name_buf, 
				_type, 
				_length, 
				*((int *)pl_data) );

		} else if (_type == SDS_STRUCT) {
			fprintf(stdout, "%-16s : (%02d) : %03d\n", 
				name_buf, 
				_type, 
				_length);

			/* now the encapsulated datums... */
			indent++;
			print_record(pl_data, _length);
			indent--;
		} else {
			/* generic data (string text) */
			fprintf(stdout, "%-16s : (%02d) : %03d\t'%s'\n", 
				name_buf, 
				_type, 
				_length, 
				(char *)pl_data);
		}
		
		ct += _length;

	} while (ct < sz);
	return;
}


/* print_sds_record
 *
 * Entry function for the recursive print_record function
 */
void print_sds_record(struct sds_record *sds) 
{
	if (sds == NULL) {
		return;
	}

	/* format header as expected by downstream parsers for reinjection */
	fprintf(stdout,
		"SDS header(0x%x) (0x%04x) pos 0x%08x sz 0x%04x (%d) bytes\n", 
		sds->header, 
		sds->h_spacer,
		sds->offset, 
		sds->size, 
		sds->size);
	print_record(sds->data_buffer, sds->size);
	fprintf(stdout, "\n");
	return;
}


/* get_record
 *  - fd     : file descriptor
 *  - offset : offset of the end of the parse attempt
 *  - f_size : file size; to prevent offset overrun
 *
 * read the next record from the file; tries to identify a header, then
 * reads the additional data expected for the whole record
 *
 * binary header format:
 *  4 bytes: header type
 *  4 bytes: header ?flags?
 *  4 bytes: message size
 *  4 bytes: zero buffer, rest of size?
 */
struct sds_record *get_record(int fd, int offset, int f_size) 
{
	if (fd <= 2) {  /* why the hell are we checking for this? */
		return NULL;
	}
#ifdef DEBUG
	fprintf(stderr, "get_record : offset: %4d\tf_size: %d\n", 
		offset, f_size);
#endif
	
	off_t o_rc = lseek(fd, (off_t)offset, SEEK_SET);
	if (o_rc != offset) {
#ifdef DEBUG
		fprintf(stderr, "lseek failed (%d)\n", (int)o_rc);
#endif
		return NULL;
	}

	// set pointers to thier locations in buffer
	unsigned char hdr_buf[SDS_HDR_SIZE];
	unsigned int *int_hdr =    (unsigned int *)&hdr_buf;
	unsigned int *hdr_spacer = (unsigned int *)&hdr_buf[4];
	unsigned int *msg_size =   (unsigned int *)&hdr_buf[8];
	unsigned int *msg_spacer = (unsigned int *)&hdr_buf[12];

	int rc;
	if ((rc = read(fd, &hdr_buf, SDS_HDR_SIZE)) != SDS_HDR_SIZE) {
#ifdef DEBUG
		fprintf(stderr, "error reading header (%d)\n", rc);
#endif
		return NULL;
	}

	// check if the header values appear to be valid
	int header_candidate = 0;	
	if (
		(*int_hdr <= HEADER_MAX) && 
		validate_h_spacer(*hdr_spacer) &&
		(*msg_spacer == 0)) 
	{
		header_candidate = 1;
	}

#ifdef DEBUG
	if (header_candidate) {
		fprintf(stderr, "Header candidate (0x%x)\n", offset);
		fprintf(stderr, "\t0x%08x 0x%08x 0x%08x 0x%08x\n",
			*int_hdr, *hdr_spacer, *msg_size, *msg_spacer);
	}
#ifdef DEBUG_LONG
	// can no longer imagine a scenario where this output is useful
	fprintf(stderr, "hdr: %-15lu sp: %-15lu sz: %-15lu sp: %-15lu\n", 
		(unsigned long)*int_hdr, (unsigned long)*hdr_spacer, 
		(unsigned long)*msg_size, (unsigned long)*msg_spacer);
#endif
#endif

	/* if we get some good test but is still invalid, then it may be
	 * a corrupted header - output the offset for later analysis */
	if ( !header_candidate && (
		(*int_hdr < HEADER_MAX) || 
		((*msg_size + offset) < f_size) ||
		validate_h_spacer(*hdr_spacer))) 
	{
#ifdef DEBUG
		fprintf(stderr, "suspected corrupt header (0x%x)\n", offset);
		fprintf(stderr, "%s\t0x%08x\t(0x%x) (0x%x)\n",
			"hdr_can", offset, *int_hdr, *msg_size);
#endif
		return NULL;
	}
#ifdef DEBUG
	fprintf(stderr, "%s: 0x%08x\n", "hdr", offset);
#endif

	// free the record after validation and printing
	struct sds_record *sds = (struct sds_record *)malloc(
		sizeof(struct sds_record) + (size_t)(*msg_size));
	sds->header = *int_hdr;
	sds->h_spacer = *hdr_spacer;
	sds->offset = offset;
	sds->size = *msg_size;

	rc = read(fd, sds->data_buffer, *msg_size);
	if (rc != *msg_size) {
		free(sds);
		sds = NULL;
	}

	return sds;
}


/* main is maina
 * - manage the sds file
 * - runs the main loop throught file
 * - manages the file offset
 */
int main(int argc, char **argv)
{

	if (argc <= 0) {
		printf("usage : sds_recover <filename>\n");
		exit(0);
	}
#if 0
	int u_int = sizeof(unsigned int);
	int u_long = sizeof(unsigned long);
	int u_longlong = sizeof(unsigned long long);
	printf("Host bytes sizes:\nui  : %d\nul  : %d\null : %d\n",
		u_int, u_long, u_longlong);
#endif
	
	/* verify that the file exists and is readable */
	struct stat stat_buffer;
	int s_rc = stat(argv[(argc - 1)], &stat_buffer);
	if (s_rc != 0) {
		fprintf(stderr, "Failed to open '%s' (%d)\n", 
			argv[(argc - 1)], errno);
		exit(1);
	}
	fprintf(stderr, "\nOpened file %s size (%08x) %d bytes\n", 
		argv[(argc - 1)], 
		(int)stat_buffer.st_size, 
		(int)stat_buffer.st_size);
	
	int fh = open(argv[(argc - 1)], O_RDONLY);
	if (fh == -1) {
		fprintf(stderr, "Error opening file '%s' (%d)\n", 
			argv[(argc - 1)],
			errno);
		exit(1);
	}

	int _searching = 1; // actively looking for a valid header
	int offset = 0;
        
	int record_count = 0;
	int corruption_count = 0;

        /* main parsing loop 
         * - reads initial 16 bytes
         * - get_record tests header validity and fills out sds record
         * - validate_sds_data verifies the internal data sanity
         * - print out the record to stdout for reinjection scripts
         * - offset/location records for corrupted areas writted to stderr
         */
	while (1) {

		if (offset >= stat_buffer.st_size) {
			fprintf(stderr, "End of file reached\n");
			break;
		}
		
		/* check for a valid header; continue on NULL */	
		struct sds_record *next_sds = 
			get_record(fh, offset, stat_buffer.st_size);

		if (next_sds == NULL) {
			if (_searching == 0) {
				corruption_count++;
				fprintf(stderr, "\thdr_cor err  (0x%08x)\n", 
					offset);
			}
			_searching = 1;
			offset++;
			continue;
		} 

		/* Header checks have passed - validate the internal data */
		int sds_error = validate_sds_data(next_sds);
		if (sds_error == 0) {
			if (_searching == 1) {
				fprintf(stderr, "\tvalid hdr    (0x%08x)\n", 
					offset);
			}
			_searching = 0;
		}

		/* if there's an internal data error, output the error to
		 * stderr, otherwise, send the sds record to stdout */
		if (sds_error != 0) {
			fprintf(stderr, "\tsds_data err (0x%08x) @(0x%x)\n", 
				offset, (next_sds->size - sds_error));
			corruption_count++;
			_searching = 1;
		} else {
			fprintf(stdout, "%08d - ", record_count++);
			print_sds_record(next_sds);
		}

		/* close out the record and push the offset forward 
		 * (payload + header size - trailing error data) */
		offset += (next_sds->size + SDS_HDR_SIZE - sds_error);
#ifdef DEBUG
		fprintf(stderr, "hdr closed 0x%08x\n\n", (offset - 1));
#endif
		free(next_sds);
		
	}
	fprintf(stderr, "\ncorruption count (%d)\n", corruption_count);

	close(fh);
	return(0);
}
