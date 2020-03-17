#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/types.h>
#include <pwd.h>
#include <lzo/lzoconf.h>
#include <lzo/lzo1x.h>
#include <arpa/inet.h>
#include <getopt.h>
#include <openssl/rsa.h>
#include <openssl/pem.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/hmac.h>
#include <openssl/ui.h>

// TAR header format
struct tarhead {
    char filename[100];    //   0 - 99
    char mode[8];                   // 100 - 107
    char nuid[8];                   // 108 - 115
    char ngid[8];                   // 116 - 123
    char size[12];                  // 124 - 135
    char modtime[12];               // 136 - 147
    char chksum[8];                 // 148 - 155
    char ftype[1];                  // 156
    char linktarget[100];  // 157 - 256
    char ustar[6];                  // 257 - 262
    char ustarver[2];               // 263 - 264
    char auid[32];                  // 265 - 296
    char agid[32];                  // 297 - 328
    char devmaj[8];                 // 329 - 336
    char devmin[8];                 // 337 - 344
    union {
	char fileprefix[155];       // 345 - 499
	struct {
	    char reserved1[41];     // 345 - 385
	    struct {
		char offset[12];    // 386 - 397, 410 - 421, ...
		char size[12];      // 398 - 409, 422 - 431, ...
	    } sd[4];                // 386 - 481 -- GNU TAR sparse file extension
	    char isextended;        // 482
	    char realsize[12];      // 483 - 494
	    char reserved2[5];      // 495 - 499
	} sph;   // sparse header   // 345 - 499
    } u;
    char reserved[12];              // 500 - 511
};

// GNU TAR sparse file extension, extended sparse header
struct speh {
    struct {
	char offset[12];
	char size[12];
    } sd[21];                       // 0 - 503
    char isextended;                // 504
    char reserved[8];               // 505 - 512
};  // sparse extended header


// Structure holding file metadata
struct filespec {
    char ftype;
    int mode;
    char devid[33];
    char inode[33];
    char auid[33];
    int nuid;
    char agid[33];
    int ngid;
    unsigned long long int filesize;
    unsigned long long int sparse_realsize;
    time_t modtime;
    char *filename;
    char *linktarget;
    char *xheader;
    int xheaderlen;
    struct sparsedata *sparsedata;
    int n_sparsedata;
    int pax;
    size_t (*c_fwrite)();
    void *c_write_handle;
    size_t (*c_fread)();
    void *c_read_handle;
};

struct sparsedata {
    unsigned long long int offset;
    unsigned long long int size;
};

struct lzop_file {
    char *buf;
    char *bufp;
    char *cbuf;
    lzo_uint bufsize;
    lzo_uint cbufsize;
    size_t (*c_fwrite)();
    size_t (*c_fread)();
    void *c_handle;
    unsigned char *working_memory;
};
#define F_H_FILTER      0x00000800L

struct tarsplit_file {
    char *buf;
    char *bufp;
    size_t bufsize;
    char *basename_path;
    size_t (*c_fread)();
    size_t (*c_fwrite)();
    void *c_handle;
    int segn;
    int finalseg;
    int segremaining;
    int segsize;
    struct filespec *orig_fs;
    char *xheader;
    int xheaderlen;
    unsigned char hmac[EVP_MAX_MD_SIZE * 2];
};

struct rsa_file {
    unsigned char *buf;
    unsigned char *bufp;
    unsigned char *outbuf;
    unsigned char *inbuf;
    size_t bufsize;
    size_t bufused;
    size_t outbufsize;
    size_t inbufsize;
    int eof;
    char mode;
    size_t (*c_fwrite)();
    size_t (*c_fread)();
    void *c_handle;
    unsigned char *ek;
    unsigned char iv[EVP_MAX_IV_LENGTH];
    int eklen;
    EVP_CIPHER_CTX *ctx;
    EVP_PKEY *evp_keypair;
};

struct key_st {
    char *eprvkey;
    char *pubkey;
    char *fingerprint;
    EVP_PKEY *evp_keypair;
    char *hmac_key_b64;
    char *hmac_key_enc_b64;
    unsigned char hmac_key[33];
    char *comment;
};

struct rsa_keys {
    int numkeys;
    struct key_st *keys;
};

struct tar_maxread_st {
    size_t max;
    size_t sz;
    size_t (*c_fread)();
    void *c_handle;
};

int tarencrypt(int argc, char **argv);
int tardecrypt();
int tar_get_next_hdr(struct filespec *fs);
int tar_write_next_hdr(struct filespec *fs);
int fsinit(struct filespec *fs, size_t (*c_fread)(), size_t (*c_fwrite)(), void *c_read_handle, void *c_write_handle);
int fsclear(struct filespec *fs);
int fsfree(struct filespec *fs);
int fsdup(struct filespec *tsf, struct filespec *sfs);
void *dmalloc(size_t size);
void dfree(void *b);
void *drealloc(void *b, size_t size) ;
size_t dmalloc_size(void *b);
char *strncpya0(char **dest, const char *src, size_t n);
char *strcata(char **dest, const char *src);
void *memcpya(void **dest, void *src, size_t n);
int getpaxvar(char *paxdata, int paxlen, char *name, char **rvalue, int *rvaluelen);
int cmpspaxvar(char *paxdata, int paxlen, char *name, char *invalue);
int setpaxvar(char **paxdata, int *paxlen, char *inname, char *invalue, int invaluelen);
int delpaxvar(char **paxdata, int *paxlen, char *inname);
unsigned int ilog10(unsigned long long int n);
char *ulli2g(unsigned long long int v, char *p);
unsigned long long int g2ulli(char *p);
size_t fwritec(const void *ptr, size_t size, size_t nmemb, size_t (*c_fwrite)(), void *c_handle, uint32_t *chksum);
struct lzop_file *lzop_init(size_t (*c_fwrite)(), void *c_handle);
struct lzop_file *lzop_init_r(size_t (*c_fread)(), void *c_handle);
size_t lzop_write(void *buf, size_t sz, size_t count, struct lzop_file *cfile);
size_t lzop_read(void *buf, size_t sz, size_t count, struct lzop_file *cfile);
int lzop_finalize(struct lzop_file *cfile);
int lzop_finalize_r(struct lzop_file *cfile);
struct rsa_file *rsa_file_init(char mode, EVP_PKEY *evp_keypair, size_t (*c_ffunc)(), void *c_handle);
size_t rsa_write(void *buf, size_t sz, size_t count, struct rsa_file *rcf);
int rsa_file_finalize(struct rsa_file *rcf);
size_t rsa_read(void *buf, size_t sz, size_t count, struct rsa_file *rcf);
uint32_t *htonlp(uint32_t v);
uint16_t *htonsp(uint16_t v);
EVP_PKEY *rsa_getkey(char mode, struct key_st *k);
void openssl_err();
struct key_st *load_keyfile(char *keyfilename);
unsigned char *sha256_digest(char *msg);
unsigned char *sha256_hex(char *msg);
int load_pkey(struct rsa_keys **rsa_keys, char *pubkey_fingerprint, char *eprivkey, char *keycomment);
int get_pkey(struct rsa_keys *rsa_keys, char *fp);

struct tarsplit_file *tarsplit_init(size_t (*c_fwrite)(), void *c_handle, char *basename_path, size_t bufsize, struct filespec *fs);
struct tarsplit_file *tarsplit_init_r(size_t (*c_fwrite)(), void *c_handle);
size_t tarsplit_write(void *buf, size_t sz, size_t count, struct tarsplit_file *tsf);
size_t tarsplit_read(void *buf, size_t sz, size_t count, struct tarsplit_file *tsf);
int tarsplit_finalize(struct tarsplit_file *tsf);
int tarsplit_finalize_r(struct tarsplit_file *tsf);
int genkey(int argc, char **argv);
int get_pkey(struct rsa_keys *rsa_keys, char *fp);
struct tar_maxread_st *tar_maxread_init(size_t sz, size_t (*c_ffunc)(), void *c_handle);
size_t tar_maxread(void *buf, size_t sz, size_t count, struct tar_maxread_st *tmr);
int tar_maxread_finalize(struct tar_maxread_st *tmr);
int encode_block_16(unsigned char *r, unsigned char *s, int c);
int decode_block_16(unsigned char *r, unsigned char *s, int c);
int gen_sparse_data_string(struct filespec *fs, char **sparsetext);
int c_fread_sparsedata(size_t (*c_ffunc)(), void *c_handle, struct filespec *fs);
int get_passwd(char *buf, int size, int rwflag, void *prompt);