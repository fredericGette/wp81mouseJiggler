#pragma once
// https://github.com/cpereida/AES128-GCM

/* Under the 16-byte key at k, encrypt the 16-byte plaintext at p and store it at c. */
void aes128e(unsigned char *c, const unsigned char *p, const unsigned char *k);

/*
* Compute the "confirm value" of the pairing process using the "Just Works" key.
* MSB (Most Significant Byte) first
* r = Mrand (128bits)
* preq = Pairing Request command (56bits)
* pres = Pairing Response command (56bits)
* iat = initiating device address type (8bits)
* ia = initiating device address (48bits)
* rat = responding device address type (8bits)
* ra = responding devices address (48bits)
* (out) confirmValue (128bits)
*/
void computeConfirmValue(BYTE *r, BYTE *preq, BYTE *pres, BYTE iat, BYTE *ia, BYTE rat, BYTE *ra, BYTE *confirmValue);

/*
* Compute the Short Term Key (STK) for the "LE legacy pairing"
* sRand = Pairing Random of the slave/responder device (128bits)
* mRand = Pairing Random of the master/initiator device (128bits)
* (out) stkValue (128bits)
*/
void computeStk(BYTE *sRand, BYTE *mRand, BYTE *stkValue);