#ifndef J606F_INFLATE_H_
#define J606F_INFLATE_H_
int RawInflate(unsigned char *dest, unsigned long long *destlen, const unsigned char *source, unsigned long long *sourcelen);
unsigned long long GzipSize(const unsigned char *src, unsigned long long len);
int GunzipBuf(unsigned char *dest, unsigned long long *destlen, const unsigned char *src, unsigned long long len);
unsigned int Crc32(const unsigned char *p, unsigned long long n);
#endif
