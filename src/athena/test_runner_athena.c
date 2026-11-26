#include <stdio.h>
#include <string.h>
#include "aws_sha256.h"
static int check_hex(const unsigned char*d,const char*expected,const char*name){char h[65];aws_hex(d,32,h);if(strcmp(h,expected)){fprintf(stderr,"FAIL %s: %s\n",name,h);return 1;}printf("PASS %s\n",name);return 0;}
int main(void){int failures=0;unsigned char d[32];aws_sha256((const unsigned char*)"abc",3,d);failures+=check_hex(d,"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad","SHA-256");aws_hmac_sha256((const unsigned char*)"Jefe",4,(const unsigned char*)"what do ya want for nothing?",28,d);failures+=check_hex(d,"5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843","HMAC-SHA256");return failures?1:0;}
