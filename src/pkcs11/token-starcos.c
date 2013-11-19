/**
 * SmartCard-HSM PKCS#11 Module
 *
 * Copyright (c) 2013, CardContact Systems GmbH, Minden, Germany
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of CardContact Systems GmbH nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL CardContact Systems GmbH BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * @file    token-starcos.c
 * @author  Andreas Schwier
 * @brief   Basic Token implementation for a Starcos card
 */

#include <string.h>
#include "token-starcos.h"

#include "bytestring.h"

#include <pkcs11/slot.h>
#include <pkcs11/object.h>
#include <pkcs11/token.h>
#include <pkcs11/certificateobject.h>
#include <pkcs11/privatekeyobject.h>
#include <pkcs11/publickeyobject.h>
#include <pkcs11/strbpcpy.h>
#include <pkcs11/asn1.h>
#include <pkcs11/pkcs15.h>
#include <pkcs11/debug.h>



static unsigned char algo_PKCS15[] =           { 0x89, 0x02, 0x13, 0x23 };
static unsigned char algo_PSS_SHA1[] =         { 0x89, 0x03, 0x13, 0x33, 0x10 };
static unsigned char algo_PSS_SHA224[] =       { 0x89, 0x03, 0x13, 0x33, 0x60 };
static unsigned char algo_PSS_SHA256[] =       { 0x89, 0x03, 0x13, 0x33, 0x30 };
static unsigned char algo_PSS_SHA384[] =       { 0x89, 0x03, 0x13, 0x33, 0x40 };
static unsigned char algo_PSS_SHA512[] =       { 0x89, 0x03, 0x13, 0x33, 0x50 };
static unsigned char algo_SHA1[] =             { 0x89, 0x02, 0x14, 0x10 };
static unsigned char algo_SHA224[] =           { 0x89, 0x02, 0x14, 0x60 };
static unsigned char algo_SHA256[] =           { 0x89, 0x02, 0x14, 0x30 };
static unsigned char algo_SHA384[] =           { 0x89, 0x02, 0x14, 0x40 };
static unsigned char algo_SHA512[] =           { 0x89, 0x02, 0x14, 0x50 };
static unsigned char algo_PKCS15_DECRYPT[] =   { 0x89, 0x02, 0x11, 0x31 };
static unsigned char algo_OAEP_DECRYPT[] =     { 0x89, 0x02, 0x11, 0x32 };


static const CK_MECHANISM_TYPE p11MechanismList[] = {
		CKM_RSA_PKCS,
		CKM_RSA_PKCS_OAEP,
		CKM_SHA1_RSA_PKCS,
		CKM_SHA224_RSA_PKCS,
		CKM_SHA256_RSA_PKCS,
		CKM_SHA384_RSA_PKCS,
		CKM_SHA512_RSA_PKCS,
		CKM_SHA1_RSA_PKCS_PSS,
		CKM_SHA224_RSA_PKCS_PSS,
		CKM_SHA256_RSA_PKCS_PSS,
		CKM_SHA384_RSA_PKCS_PSS,
		CKM_SHA512_RSA_PKCS_PSS
};



static struct starcosPrivateData *getPrivateData(struct p11Token_t *token)
{
	return (struct starcosPrivateData *)(token + 1);
}



static struct p11Token_t *getBaseToken(struct p11Token_t *token)
{
	if (!token->slot->primarySlot)
		return token;
	return token->slot->primarySlot->token;
}



static void lock(struct p11Token_t *token)
{
	struct starcosPrivateData *sc;

	FUNC_CALLED();

	sc = getPrivateData(getBaseToken(token));
	p11LockMutex(sc->mutex);

#ifdef DEBUG
	debug("Lock released\n");
#endif
}



static void unlock(struct p11Token_t *token)
{
	struct starcosPrivateData *sc;

	FUNC_CALLED();

	sc = getPrivateData(getBaseToken(token));
	p11UnlockMutex(sc->mutex);
}



static int selectApplication(struct p11Token_t *token)
{
	int rc, *sa;
	unsigned short SW1SW2;
	struct starcosPrivateData *sc;

	FUNC_CALLED();

	sc = getPrivateData(token);

	if (token->slot->primarySlot) {
		sa = &(getPrivateData(getBaseToken(token))->selectedApplication);
	} else {
		sa = &sc->selectedApplication;
	}

	if (sc->application->aidId == *sa) {
		return 0;
	}

	rc = transmitAPDU(token->slot, 0x00, 0xA4, 0x04, 0x0C,
			sc->application->aid.len, sc->application->aid.val,
			0, NULL, 0, &SW1SW2);

	if (rc < 0) {
		FUNC_FAILS(rc, "transmitAPDU failed");
	}

	if (SW1SW2 != 0x9000) {
		FUNC_FAILS(-1, "Selecting application failed");
	}

	*sa = sc->application->aidId;

	FUNC_RETURNS(0);
}



static int readCertEF(struct p11Slot_t *slot, bytestring fid, unsigned char *content, size_t len)
{
	int rc, le, ne, ofs, maxapdu;
	unsigned short SW1SW2;
	unsigned char *po;

	FUNC_CALLED();

	// Select EF
	rc = transmitAPDU(slot, 0x00, 0xA4, 0x02, 0x0C,
			fid->len, fid->val,
			0, NULL, 0, &SW1SW2);

	if (rc < 0) {
		FUNC_FAILS(rc, "transmitAPDU failed");
	}

	if (SW1SW2 != 0x9000) {
		FUNC_FAILS(-1, "File not found");
	}

	// Read first 5 bytes to determine tag and length
	ofs = 0;
	rc = transmitAPDU(slot, 0x00, 0xB0, ofs >> 8, ofs & 0xFF,
			0, NULL,
			5, content + ofs, len - ofs, &SW1SW2);

	if (rc < 0) {
		FUNC_FAILS(rc, "transmitAPDU failed");
	}

	if (SW1SW2 != 0x9000) {
		FUNC_FAILS(-1, "Read EF failed");
	}

	ofs += rc;

	// Restrict the number of bytes in Le to either the maximum APDU size of STARCOS or
	// the maximum APDU size of the reader, if any.
	maxapdu = 1920;
	if (slot->maxRAPDU && (slot->maxRAPDU < maxapdu))
		maxapdu = slot->maxRAPDU;
	maxapdu -= 2;		// Accommodate SW1/SW2

	le = 65536;			// Read all if no certificate found
	if (*content == 0x30) {
		po = content;
		asn1Tag(&po);
		rc = asn1Length(&po);
		rc += po - content;
		le = rc - ofs;
	}

	do	{
		ne = le;
		// Restrict Ne to the maximum APDU length allowed
		if (((le != 65536) || slot->noExtLengthReadAll) && (le > maxapdu))
			ne = maxapdu;

		rc = transmitAPDU(slot, 0x00, 0xB0, ofs >> 8, ofs & 0xFF,
				0, NULL,
				ne, content + ofs, len - ofs, &SW1SW2);

		if (rc < 0) {
			FUNC_FAILS(rc, "transmitAPDU failed");
		}

		if ((SW1SW2 != 0x9000) && (SW1SW2 != 0x6B00) && (SW1SW2 != 0x6282)) {
			FUNC_FAILS(-1, "Read EF failed");
		}
		ofs += rc;
		if (le != 65536)
			le -= rc;
	} while ((rc > 0) && (ofs < len) && (le > 0));

	FUNC_RETURNS(ofs);
}



static int determinePinUseCounter(struct p11Slot_t *slot, unsigned char recref, int *useCounter, int *lifeCycle)
{
	int rc;
	unsigned short SW1SW2;
	unsigned char rec[256], *p;
	FUNC_CALLED();

	// Select EF
	rc = transmitAPDU(slot, 0x00, 0xA4, 0x02, 0x0C,
			2, (unsigned char *)"\x00\x13",
			0, NULL, 0, &SW1SW2);

	if (rc < 0) {
		FUNC_FAILS(rc, "transmitAPDU failed");
	}

	if (SW1SW2 != 0x9000) {
		FUNC_FAILS(-1, "File not found");
	}

	// Read record, but leave 3 bytes to add encapsulating 30 81 FF later
	rc = transmitAPDU(slot, 0x00, 0xB2, recref, 0x04,
			0, NULL,
			0, rec, sizeof(rec) - 3, &SW1SW2);

	if (rc < 0) {
		FUNC_FAILS(rc, "transmitAPDU failed");
	}

	if (SW1SW2 != 0x9000) {
		FUNC_FAILS(-1, "File not found");
	}

	rc = asn1Encap(0x30, rec, rc);
	rc = asn1Validate(rec, rc);

	if (rc > 0) {
		FUNC_FAILS(rc, "ASN.1 structure invalid");
	}

	*useCounter = 0;
	p = asn1Find(rec, (unsigned char *)"\x30\x7B\xA4\x9F\x22", 4);

	if (p) {
		asn1Tag(&p);
		asn1Length(&p);

		*useCounter = (*p == 0xFF ? 0 : *p);
	}

	p = asn1Find(rec, (unsigned char *)"\x30\x8A", 2);

	if (p) {
		asn1Tag(&p);
		asn1Length(&p);

		*lifeCycle = *p;
	}

	FUNC_RETURNS(CKR_OK);
}



static int checkPINStatus(struct p11Slot_t *slot, unsigned char pinref)
{
	int rc;
	unsigned short SW1SW2;
	FUNC_CALLED();

	rc = transmitAPDU(slot, 0x00, 0x20, 0x00, pinref,
			0, NULL,
			0, NULL, 0, &SW1SW2);

	if (rc < 0) {
		FUNC_FAILS(rc, "transmitAPDU failed");
	}

	FUNC_RETURNS(SW1SW2);
}



static int getSignatureSize(CK_MECHANISM_TYPE mech, struct p11Object_t *pObject)
{
	switch(mech) {
	case CKM_RSA_PKCS:
	case CKM_SHA1_RSA_PKCS:
	case CKM_SHA224_RSA_PKCS:
	case CKM_SHA256_RSA_PKCS:
	case CKM_SHA384_RSA_PKCS:
	case CKM_SHA512_RSA_PKCS:
	case CKM_SHA1_RSA_PKCS_PSS:
	case CKM_SHA224_RSA_PKCS_PSS:
	case CKM_SHA256_RSA_PKCS_PSS:
	case CKM_SHA384_RSA_PKCS_PSS:
	case CKM_SHA512_RSA_PKCS_PSS:
		return pObject->keysize >> 3;
	default:
		return -1;
	}
}


static int getAlgorithmIdForSigning(struct p11Token_t *token, CK_MECHANISM_TYPE mech, unsigned char **algotlv)
{
	switch(mech) {
	case CKM_RSA_PKCS:
	case CKM_SHA1_RSA_PKCS:
	case CKM_SHA224_RSA_PKCS:
	case CKM_SHA256_RSA_PKCS:
	case CKM_SHA384_RSA_PKCS:
	case CKM_SHA512_RSA_PKCS:
		*algotlv = algo_PKCS15;
		break;
	case CKM_SHA1_RSA_PKCS_PSS:
		*algotlv = algo_PSS_SHA1;
		break;
	case CKM_SHA224_RSA_PKCS_PSS:
		*algotlv = algo_PSS_SHA224;
		break;
	case CKM_SHA256_RSA_PKCS_PSS:
		*algotlv = algo_PSS_SHA256;
		break;
	case CKM_SHA384_RSA_PKCS_PSS:
		*algotlv = algo_PSS_SHA384;
		break;
	case CKM_SHA512_RSA_PKCS_PSS:
		*algotlv = algo_PSS_SHA512;
		break;
	default:
		return CKR_MECHANISM_INVALID;
	}

	return CKR_OK;
}



static int getAlgorithmIdForDigest(struct p11Token_t *token, CK_MECHANISM_TYPE mech, unsigned char **algotlv)
{
	switch(mech) {
	case CKM_SHA1_RSA_PKCS:
	case CKM_SHA1_RSA_PKCS_PSS:
		*algotlv = algo_SHA1;
		break;
	case CKM_SHA224_RSA_PKCS:
	case CKM_SHA224_RSA_PKCS_PSS:
		*algotlv = algo_SHA224;
		break;
	case CKM_SHA256_RSA_PKCS:
	case CKM_SHA256_RSA_PKCS_PSS:
		*algotlv = algo_SHA256;
		break;
	case CKM_SHA384_RSA_PKCS:
	case CKM_SHA384_RSA_PKCS_PSS:
		*algotlv = algo_SHA384;
		break;
	case CKM_SHA512_RSA_PKCS:
	case CKM_SHA512_RSA_PKCS_PSS:
		*algotlv = algo_SHA512;
		break;
	default:
		return CKR_MECHANISM_INVALID;
	}

	return CKR_OK;
}



static int getAlgorithmIdForDecryption(CK_MECHANISM_TYPE mech, unsigned char **algotlv)
{
	switch(mech) {
	case CKM_RSA_PKCS:
		*algotlv = algo_PKCS15_DECRYPT;
		break;
	case CKM_RSA_PKCS_OAEP:
		*algotlv = algo_OAEP_DECRYPT;
		break;
	default:
		return CKR_MECHANISM_INVALID;
	}
	return CKR_OK;
}



/**
 * Update internal PIN status based on SW1/SW2 received from token
 */
static int updatePinStatus(struct p11Token_t *token, int pinstatus)
{
	int rc = CKR_OK;

	token->info.flags &= ~(CKF_TOKEN_INITIALIZED | CKF_USER_PIN_INITIALIZED | CKF_USER_PIN_FINAL_TRY | CKF_USER_PIN_LOCKED | CKF_USER_PIN_COUNT_LOW | CKF_USER_PIN_TO_BE_CHANGED );

	if (pinstatus != 0x6984) {
		token->info.flags |= CKF_TOKEN_INITIALIZED | CKF_USER_PIN_INITIALIZED;
	}

	if (token->pinChangeRequired) {
		token->info.flags |= CKF_USER_PIN_TO_BE_CHANGED;
	}

	switch(pinstatus) {
	case 0x9000:
		rc = CKR_OK;
		break;
	case 0x6984:
		rc = CKR_USER_PIN_NOT_INITIALIZED;
		break;
	case 0x6983:
	case 0x63C0:
		token->info.flags |= CKF_USER_PIN_LOCKED;
		rc = CKR_PIN_LOCKED;
		break;
	case 0x63C1:
		token->info.flags |= CKF_USER_PIN_FINAL_TRY|CKF_USER_PIN_COUNT_LOW;
		rc = CKR_PIN_INCORRECT;
		break;
	case 0x63C2:
		token->info.flags |= CKF_USER_PIN_COUNT_LOW;
		rc = CKR_PIN_INCORRECT;
		break;
	default:
		rc = CKR_PIN_INCORRECT;
		break;
	}
	return rc;
}



static int digest(struct p11Token_t *token, CK_MECHANISM_TYPE mech, unsigned char *data, size_t len)
{
	int rc,chunk;
	unsigned short SW1SW2;
	unsigned char scr[1008],*algo, *po;

	FUNC_CALLED();

	rc = getAlgorithmIdForDigest(token, mech, &algo);
	if (rc != CKR_OK) {
		FUNC_FAILS(rc, "getAlgorithmIdForDigest() failed");
	}

	po = algo;
	asn1Tag(&po);
	rc = asn1Length(&po);
	rc += po - algo;

	rc = transmitAPDU(token->slot, 0x00, 0x22, 0x41, 0xAA,
		rc, algo,
		0, NULL, 0, &SW1SW2);

	if (rc < 0) {
		FUNC_FAILS(CKR_DEVICE_ERROR, "transmitAPDU failed");
	}

	if (SW1SW2 != 0x9000) {
		FUNC_FAILS(CKR_DEVICE_ERROR, "MANAGE SE failed");
	}

	if (len <= 1000) {
		scr[0] = 0x90;
		scr[1] = 0x00;
		memcpy(scr + 2, data, len);
		rc = asn1Encap(0x80, scr + 2, len) + 2;

		rc = transmitAPDU(token->slot, 0x00, 0x2A, 0x90, 0xA0,
				rc, scr,
				0, NULL, 0, &SW1SW2);

		if (rc < 0) {
			FUNC_FAILS(CKR_DEVICE_ERROR, "transmitAPDU failed");
		}

		if (SW1SW2 != 0x9000) {
			FUNC_FAILS(CKR_DEVICE_ERROR, "Hash operation failed");
		}
	} else {
		scr[0] = 0x90;
		scr[1] = 0x00;

		rc = transmitAPDU(token->slot, 0x10, 0x2A, 0x90, 0xA0,
				2, scr,
				0, NULL, 0, &SW1SW2);

		if (rc < 0) {
			FUNC_FAILS(CKR_DEVICE_ERROR, "transmitAPDU failed");
		}

		if (SW1SW2 != 0x9000) {
			FUNC_FAILS(CKR_DEVICE_ERROR, "Hash operation failed");
		}

		while (len > 0) {
			// Chunk must be aligned to the hash block size
			// As we support SHA-2 up to 512 we choose 7 * 128 as chunk size
			chunk = (len > 896 ? 896 : len);

			memcpy(scr, data, chunk);
			rc = asn1Encap(0x80, scr, chunk);

			rc = transmitAPDU(token->slot, len > chunk ? 0x10 : 0x00, 0x2A, 0x90, 0xA0,
					rc, scr,
					0, NULL, 0, &SW1SW2);

			if (rc < 0) {
				FUNC_FAILS(CKR_DEVICE_ERROR, "transmitAPDU failed");
			}

			if (SW1SW2 != 0x9000) {
				FUNC_FAILS(CKR_DEVICE_ERROR, "Hash operation failed");
			}

			len -= chunk;
			data += chunk;
		}
	}

	return CKR_OK;
}



static int starcos_C_SignInit(struct p11Object_t *pObject, CK_MECHANISM_PTR mech)
{
	unsigned char *algotlv;

	FUNC_CALLED();

	FUNC_RETURNS(getAlgorithmIdForSigning(pObject->token, mech->mechanism, &algotlv));
}



static int starcos_C_Sign(struct p11Object_t *pObject, CK_MECHANISM_TYPE mech, CK_BYTE_PTR pData, CK_ULONG ulDataLen, CK_BYTE_PTR pSignature, CK_ULONG_PTR pulSignatureLen)
{
	int rc, len, signaturelen;
	unsigned short SW1SW2;
	unsigned char scr[256],*s, *d;
	struct p11Slot_t *slot;

	FUNC_CALLED();

	rc = getSignatureSize(mech, pObject);
	if (rc < 0) {
		FUNC_FAILS(CKR_MECHANISM_INVALID, "Unknown mechanism");
	}
	signaturelen = rc;

	if (pSignature == NULL) {
		*pulSignatureLen = signaturelen;
		FUNC_RETURNS(CKR_OK);
	}

	if (*pulSignatureLen < signaturelen) {
		*pulSignatureLen = signaturelen;
		FUNC_FAILS(CKR_BUFFER_TOO_SMALL, "Signature length is larger than buffer");
	}

	slot = pObject->token->slot;
	lock(pObject->token);
	if (!slot->token) {
		FUNC_RETURNS(CKR_DEVICE_REMOVED);
	}

	rc = selectApplication(pObject->token);
	if (rc < 0) {
		unlock(pObject->token);
		FUNC_FAILS(CKR_DEVICE_ERROR, "selecting application failed");
	}

	if (mech != CKM_RSA_PKCS) {
		rc = digest(pObject->token, mech, pData, ulDataLen);
		if (rc != CKR_OK) {
			unlock(pObject->token);
			FUNC_FAILS(rc, "digesting failed");
		}
		pData = NULL;
		ulDataLen = 0;
	}

	rc = getAlgorithmIdForSigning(pObject->token, mech, &s);
	if (rc != CKR_OK) {
		unlock(pObject->token);
		FUNC_FAILS(rc, "getAlgorithmIdForSigning() failed");
	}

	d = scr;
	*d++ = *s++;
	len = *s;
	*d++ = *s++;
	while (len--) {
		*d++ = *s++;
	}
	*d++ = 0x84;
	*d++ = 0x01;
	*d++ = (unsigned char)pObject->tokenid;

	rc = transmitAPDU(pObject->token->slot, 0x00, 0x22, 0x41, 0xB6,
		d - scr, scr,
		0, NULL, 0, &SW1SW2);

	if (rc < 0) {
		unlock(pObject->token);
		FUNC_FAILS(CKR_DEVICE_ERROR, "transmitAPDU failed");
	}

	if (SW1SW2 != 0x9000) {
		unlock(pObject->token);
		FUNC_FAILS(CKR_DEVICE_ERROR, "MANAGE SE failed");
	}

	rc = transmitAPDU(pObject->token->slot, 0x00, 0x2A, 0x9E, 0x9A,
			ulDataLen, pData,
			0, pSignature, *pulSignatureLen, &SW1SW2);

	if (rc < 0) {
		unlock(pObject->token);
		FUNC_FAILS(CKR_DEVICE_ERROR, "transmitAPDU failed");
	}

	if (SW1SW2 == 0x6982) {
		unlock(pObject->token);
		FUNC_FAILS(CKR_USER_NOT_LOGGED_IN, "User not logged in");
	}

	if (SW1SW2 != 0x9000) {
		unlock(pObject->token);
		FUNC_FAILS(CKR_DEVICE_ERROR, "Signature operation failed");
	}

	*pulSignatureLen = rc;

	unlock(pObject->token);
	FUNC_RETURNS(CKR_OK);
}



static int starcos_C_DecryptInit(struct p11Object_t *pObject, CK_MECHANISM_PTR mech)
{
	unsigned char *algotlv;

	FUNC_CALLED();

	FUNC_RETURNS(getAlgorithmIdForDecryption(mech->mechanism, &algotlv));
}



static int starcos_C_Decrypt(struct p11Object_t *pObject, CK_MECHANISM_TYPE mech, CK_BYTE_PTR pEncryptedData, CK_ULONG ulEncryptedDataLen, CK_BYTE_PTR pData, CK_ULONG_PTR pulDataLen)
{
	int rc, len;
	unsigned char *d,*s;
	unsigned short SW1SW2;
	unsigned char scr[257];
	struct p11Slot_t *slot;

	FUNC_CALLED();

	if (ulEncryptedDataLen != 256)
		FUNC_FAILS(CKR_ENCRYPTED_DATA_LEN_RANGE, "Cryptogram size must be 256 byte");

	if (pData == NULL) {
		*pulDataLen = pObject->keysize >> 3;
		FUNC_RETURNS(CKR_OK);
	}

	slot = pObject->token->slot;
	lock(pObject->token);
	if (!slot->token) {
		FUNC_RETURNS(CKR_DEVICE_REMOVED);
	}

	rc = selectApplication(pObject->token);
	if (rc < 0) {
		unlock(pObject->token);
		FUNC_FAILS(CKR_DEVICE_ERROR, "selecting application failed");
	}

	rc = getAlgorithmIdForDecryption(mech, &s);
	if (rc != CKR_OK) {
		unlock(pObject->token);
		FUNC_FAILS(rc, "getAlgorithmIdForDecryption() failed");
	}

	d = scr;
	*d++ = *s++;
	len = *s;
	*d++ = *s++;
	while (len--) {
		*d++ = *s++;
	}
	*d++ = 0x84;
	*d++ = 0x01;
	*d++ = (unsigned char)pObject->tokenid;

	rc = transmitAPDU(pObject->token->slot, 0x00, 0x22, 0x41, 0xB8,
		d - scr, scr,
		0, NULL, 0, &SW1SW2);

	if (rc < 0) {
		unlock(pObject->token);
		FUNC_FAILS(CKR_DEVICE_ERROR, "transmitAPDU failed");
	}

	if (SW1SW2 != 0x9000) {
		unlock(pObject->token);
		FUNC_FAILS(CKR_DEVICE_ERROR, "MANAGE SE failed");
	}

	scr[0] = 0x81;
	memcpy(scr + 1, pEncryptedData, ulEncryptedDataLen);

	rc = transmitAPDU(pObject->token->slot, 0x00, 0x2A, 0x80, 0x86,
			257, scr,
			0, scr, sizeof(scr), &SW1SW2);

	unlock(pObject->token);

	if (rc < 0) {
		FUNC_FAILS(rc, "transmitAPDU failed");
	}

	if (SW1SW2 != 0x9000) {
		FUNC_FAILS(CKR_ENCRYPTED_DATA_INVALID, "Decryption operation failed");
	}

	*pulDataLen = rc;
	if (rc > *pulDataLen) {
		FUNC_FAILS(CKR_BUFFER_TOO_SMALL, "supplied buffer too small");
	}

	memcpy(pData, scr, rc);

	FUNC_RETURNS(CKR_OK);
}



static int addCertificateObject(struct p11Token_t *token, struct p15CertificateDescription *p15)
{
	unsigned char certValue[MAX_CERTIFICATE_SIZE];
	struct p11Object_t *pObject;
	int rc;

	FUNC_CALLED();

	rc = readCertEF(token->slot, &p15->efidOrPath, certValue, sizeof(certValue));

	if (rc < 0) {
		FUNC_FAILS(CKR_DEVICE_ERROR, "Error reading certificate");
	}

	rc = createCertificateObjectFromP15(p15, certValue, rc, &pObject);

	if (rc < 0) {
		FUNC_FAILS(CKR_DEVICE_ERROR, "Could not create P11 certificate object");
	}

	addObject(token, pObject, TRUE);

	FUNC_RETURNS(CKR_OK);
}



static int addPrivateKeyObject(struct p11Token_t *token, struct p15PrivateKeyDescription *p15)
{
	CK_OBJECT_CLASS class = CKO_CERTIFICATE;
	CK_ATTRIBUTE template[] = {
			{ CKA_CLASS, &class, sizeof(class) },
			{ CKA_ID, NULL, 0 }
	};
	struct p11Object_t *p11prikey, *p11pubkey, *p11cert;
	int rc,useAA;

	FUNC_CALLED();

	template[1].pValue = p15->id.val;
	template[1].ulValueLen = p15->id.len;

	rc = findMatchingTokenObject(token, template, 2, &p11cert);

	if (rc != CKR_OK) {
		FUNC_FAILS(rc, "Could not find matching certificate");
	}

	useAA = (p15->usage & P15_NONREPUDIATION) && (token->pinUseCounter == 1);

	rc = createPrivateKeyObjectFromP15(p15, p11cert, useAA, &p11prikey);

	if (rc != CKR_OK) {
		FUNC_FAILS(CKR_DEVICE_ERROR, "Could not create private key object");
	}

	p11prikey->C_SignInit = starcos_C_SignInit;
	p11prikey->C_Sign = starcos_C_Sign;
	p11prikey->C_DecryptInit = starcos_C_DecryptInit;
	p11prikey->C_Decrypt = starcos_C_Decrypt;

	p11prikey->tokenid = p15->keyReference;
	p11prikey->keysize = p15->keysize;
	addObject(token, p11prikey, useAA ? TRUE : FALSE);

	rc = createPublicKeyObjectFromCertificate(p15, p11cert, &p11pubkey);

	if (rc != CKR_OK) {
		FUNC_FAILS(CKR_DEVICE_ERROR, "Could not create public key object");
	}

	addObject(token, p11pubkey, TRUE);

	FUNC_RETURNS(CKR_OK);
}



static int loadObjects(struct p11Token_t *token)
{
	struct starcosPrivateData *sc;
	int rc,i;

	FUNC_CALLED();

	sc = getPrivateData(token);

	for (i = 0; i < sc->application->certsLen; i++) {
		struct p15CertificateDescription *p15 = &sc->application->certs[i];

		rc = addCertificateObject(token, p15);
		if (rc != CKR_OK) {
#ifdef DEBUG
			debug("addCertificateObject failed with rc=%d\n", rc);
#endif
		}
	}

	for (i = 0; i < sc->application->privateKeysLen; i++) {
		struct p15PrivateKeyDescription *p15 = &sc->application->privateKeys[i];

		rc = addPrivateKeyObject(token, p15);
		if (rc != CKR_OK) {
#ifdef DEBUG
			debug("addPrivateKeyObject failed with rc=%d\n", rc);
#endif
		}
	}

	FUNC_RETURNS(CKR_OK);
}



int encodeF2B(unsigned char *pin, int pinlen, unsigned char *f2b)
{
	unsigned char *po;
	int i;

	FUNC_CALLED();

	if ((pinlen <= 4) || (pinlen > 14)) {
		FUNC_FAILS(CKR_PIN_LEN_RANGE, "PIN length must be between 4 and 14");
	}

	memset(f2b, 0xFF, 8);
	f2b[0] = 0x20 | pinlen;

	po = f2b + 1;
	for (i = 0; i < pinlen; i++) {
		if ((*pin < '0') || (*pin > '9')) {
			FUNC_FAILS(CKR_ARGUMENTS_BAD, "PIN must be numeric");
		}
		if (i & 1) {
			*po = (*po & 0xF0) | (*pin & 0x0F);
			po++;
		} else {
			*po = (*po & 0x0F) | ((*pin & 0x0F) << 4);
		}
		pin++;
	}
	return CKR_OK;
}



/**
 * Perform PIN verification and make private objects visible
 *
 * @param slot      The slot in which the token is inserted
 * @param userType  One of CKU_SO or CKU_USER
 * @param pin       Pointer to PIN value or NULL is PIN shall be verified using PIN-Pad
 * @param pinLen    The length of the PIN supplied in pin
 * @return          CKR_OK or any other Cryptoki error code
 */
static int login(struct p11Slot_t *slot, int userType, unsigned char *pin, int pinlen)
{
	int rc = CKR_OK;
	unsigned short SW1SW2;
	unsigned char f2b[8];
	struct starcosPrivateData *sc;

	FUNC_CALLED();

	lock(slot->token);
	if (!slot->token) {
		FUNC_RETURNS(CKR_DEVICE_REMOVED);
	}

	rc = selectApplication(slot->token);
	if (rc < 0) {
		unlock(slot->token);
		FUNC_FAILS(rc, "selecting application failed");
	}

	sc = getPrivateData(slot->token);

	if (userType == CKU_SO) {
		rc = encodeF2B(pin, pinlen, sc->sopin);

		if (rc != CKR_OK) {
			unlock(slot->token);
			FUNC_FAILS(rc, "Could not encode PIN");
		}
	} else {

		if (slot->hasFeatureVerifyPINDirect && !pinlen && !pin) {
#ifdef DEBUG
			debug("Verify PIN using CKF_PROTECTED_AUTHENTICATION_PATH\n");
#endif
			memset(f2b, 0xFF, 8);
			f2b[0] = 0x20;

			rc = transmitVerifyPinAPDU(slot, 0x00, 0x20, 0x00, sc->application->pinref,
					8, f2b,
					&SW1SW2,
					PIN_SYSTEM_UNIT_BYTES + PIN_POSITION_1 + PIN_LEFT_JUSTIFICATION + PIN_FORMAT_BCD, /* bmFormatString */
					0x06, 0x0F, /* Minimum and maximum length of PIN */
					0x47, /* bmPINBlockString: inserted PIN length is 4 bits, 7 bytes PIN block*/
					0x04 /* bmPINLengthFormat: system units are bits, PIN length position is 4 bits*/
					);
		} else {
#ifdef DEBUG
			debug("Verify PIN using provided PIN value\n");
#endif
			rc = encodeF2B(pin, pinlen, f2b);

			if (rc != CKR_OK) {
				unlock(slot->token);
				FUNC_FAILS(rc, "Could not encode PIN");
			}

			rc = transmitAPDU(slot, 0x00, 0x20, 0x00, sc->application->pinref,
					8, f2b,
					0, NULL, 0, &SW1SW2);
		}


		if (rc < 0) {
			unlock(slot->token);
			FUNC_FAILS(CKR_DEVICE_ERROR, "transmitAPDU failed");
		}

		rc = updatePinStatus(slot->token, SW1SW2);

		if (rc != CKR_OK) {
			unlock(slot->token);
			FUNC_FAILS(rc, "login failed");
		}
	}

	unlock(slot->token);
	FUNC_RETURNS(CKR_OK);
}



/**
 * Initialize user pin in SO session
 *
 * @param slot      The slot in which the token is inserted
 * @param pin       Pointer to PIN value or NULL if PIN shall be verified using PIN-Pad
 * @param pinLen    The length of the PIN supplied in pin
 * @return          CKR_OK or any other Cryptoki error code
 */
static int initpin(struct p11Slot_t *slot, unsigned char *pin, int pinlen)
{
	int rc = CKR_OK;
	unsigned short SW1SW2;
	unsigned char data[16], pinref;
	struct starcosPrivateData *sc;

	FUNC_CALLED();

	if (pinlen) {
		rc = encodeF2B(pin, pinlen, data + 8);

		if (rc != CKR_OK) {
			FUNC_FAILS(rc, "Could not encode PIN");
		}
	}

	lock(slot->token);
	if (!slot->token) {
		FUNC_RETURNS(CKR_DEVICE_REMOVED);
	}

	rc = selectApplication(slot->token);
	if (rc < 0) {
		unlock(slot->token);
		FUNC_FAILS(rc, "selecting application failed");
	}

	sc = getPrivateData(slot->token);
	memcpy(data, sc->sopin, sizeof(sc->sopin));
	pinref = sc->application->pinref;

#ifdef DEBUG
	debug("Init PIN using provided PIN value\n");
#endif
	if (pin) {
		rc = transmitAPDU(slot, 0x00, 0x2C, 0x00, pinref,
				sizeof(data), data,
				0, NULL, 0, &SW1SW2);
	} else {
		rc = transmitAPDU(slot, 0x00, 0x2C, 0x01, pinref,
				sizeof(sc->sopin), data,
				0, NULL, 0, &SW1SW2);
	}
	if (rc < 0) {
		unlock(slot->token);
		FUNC_FAILS(CKR_DEVICE_ERROR, "transmitAPDU failed");
	}

	if (SW1SW2 == 0x6982) {
		unlock(slot->token);
		FUNC_FAILS(CKR_KEY_FUNCTION_NOT_PERMITTED, "Function not allowed");
	}

	if (SW1SW2 != 0x9000) {
		unlock(slot->token);
		FUNC_FAILS(CKR_PIN_INCORRECT, "Invalid SO-PIN");
	}

	rc = checkPINStatus(slot, pinref);

	if (rc < 0) {
		unlock(slot->token);
		FUNC_FAILS(CKR_DEVICE_ERROR, "transmitAPDU failed");
	}

	updatePinStatus(slot->token, rc);

	unlock(slot->token);
	FUNC_RETURNS(CKR_OK);
}



/**
 * Change PIN in User or SO session
 *
 * @param slot      The slot in which the token is inserted
 * @param oldpin    Pointer to PIN value or NULL if PIN shall be verified using PIN-Pad
 * @param oldpinLen The length of the PIN supplied in oldpin
 * @param newpin    Pointer to PIN value or NULL if PIN shall be verified using PIN-Pad
 * @param newpinLen The length of the PIN supplied in newpin
 * @return          CKR_OK or any other Cryptoki error code
 */
static int setpin(struct p11Slot_t *slot, unsigned char *oldpin, int oldpinlen, unsigned char *newpin, int newpinlen)
{
	int rc = CKR_OK;
	unsigned short SW1SW2;
	unsigned char data[16];
	struct starcosPrivateData *sc;

	FUNC_CALLED();

	if (slot->token->user == CKU_SO) {
		FUNC_FAILS(CKR_USER_TYPE_INVALID, "User not logged in");
	}

	rc = encodeF2B(oldpin, oldpinlen, data);

	if (rc != CKR_OK) {
		FUNC_FAILS(rc, "Could not encode OldPIN");
	}

	rc = encodeF2B(newpin, newpinlen, data + 8);

	if (rc != CKR_OK) {
		FUNC_FAILS(rc, "Could not encode NewPIN");
	}

	lock(slot->token);
	if (!slot->token) {
		FUNC_RETURNS(CKR_DEVICE_REMOVED);
	}

	rc = selectApplication(slot->token);
	if (rc < 0) {
		unlock(slot->token);
		FUNC_FAILS(rc, "selecting application failed");
	}

	sc = getPrivateData(slot->token);

#ifdef DEBUG
	debug("Set PIN using provided PIN value\n");
#endif
	rc = transmitAPDU(slot, 0x00, 0x24, 0x00, sc->application->pinref,
		sizeof(data), data,
		0, NULL, 0, &SW1SW2);

	if (rc < 0) {
		unlock(slot->token);
		FUNC_FAILS(CKR_DEVICE_ERROR, "transmitAPDU failed");
	}

	if (slot->token->user == CKU_SO) {
		if (SW1SW2 != 0x9000) {
			unlock(slot->token);
			FUNC_FAILS(CKR_PIN_INCORRECT, "Incorrect old SO-PIN");
		}
	} else {
		slot->token->pinChangeRequired = FALSE;
		rc = updatePinStatus(slot->token, SW1SW2);
	}

	unlock(slot->token);
	FUNC_RETURNS(rc);
}



/**
 * Starcos does not support a deauthentication for the User PIN
 *
 * @param slot      The slot in which the token is inserted
 * @return          CKR_OK or any other Cryptoki error code
 */
static int logout(struct p11Slot_t *slot)
{
	struct starcosPrivateData *sc;

	FUNC_CALLED();

	sc = getPrivateData(slot->token);
	memset(sc->sopin, 0, sizeof(sc->sopin));

	FUNC_RETURNS(CKR_OK);
}



static void freeStarcosToken(struct p11Token_t *token)
{
	struct starcosPrivateData *sc;

	sc = getPrivateData(token);
	p11DestroyMutex(sc->mutex);
}



/**
 * Create a new STARCOS token if token detection and initialization is successful
 *
 * @param slot      The slot in which a token was detected
 * @param token     Pointer to pointer updated with newly created token structure
 * @return          CKR_OK or any other Cryptoki error code
 */
int createStarcosToken(struct p11Slot_t *slot, struct p11Token_t **token, struct p11TokenDriver *drv, struct starcosApplication *application)
{
	struct p11Token_t *ptoken;
	struct starcosPrivateData *sc;
	int rc, lc;

	FUNC_CALLED();

	ptoken = (struct p11Token_t *)calloc(sizeof(struct p11Token_t) + sizeof(struct starcosPrivateData), 1);

	if (ptoken == NULL) {
		FUNC_FAILS(CKR_HOST_MEMORY, "Out of memory");
	}

	ptoken->slot = slot;
	ptoken->freeObjectNumber = 1;
	strbpcpy(ptoken->info.manufacturerID, "Giesecke & Devrient", sizeof(ptoken->info.manufacturerID));
	strbpcpy(ptoken->info.model, drv->name, sizeof(ptoken->info.model));
	ptoken->info.ulFreePrivateMemory = CK_UNAVAILABLE_INFORMATION;
	ptoken->info.ulFreePublicMemory = CK_UNAVAILABLE_INFORMATION;
	ptoken->info.ulMinPinLen = 6;
	ptoken->info.ulMaxPinLen = 16;
	ptoken->info.ulTotalPrivateMemory = CK_UNAVAILABLE_INFORMATION;
	ptoken->info.ulTotalPublicMemory = CK_UNAVAILABLE_INFORMATION;
	ptoken->info.ulMaxSessionCount = CK_EFFECTIVELY_INFINITE;
	ptoken->info.ulMaxRwSessionCount = CK_EFFECTIVELY_INFINITE;
	ptoken->info.ulSessionCount = CK_UNAVAILABLE_INFORMATION;

	ptoken->info.flags = CKF_WRITE_PROTECTED;
	ptoken->user = 0xFF;
	ptoken->drv = drv;

	sc = getPrivateData(ptoken);
	sc->selectedApplication = 0;
	sc->application = application;

	p11CreateMutex(&sc->mutex);

	strbpcpy(ptoken->info.label, sc->application->name, sizeof(ptoken->info.label));

	rc = selectApplication(ptoken);

	if (rc < 0) {
		freeStarcosToken(ptoken);
		FUNC_FAILS(CKR_DEVICE_ERROR, "Application not found on token");
	}

	if (sc->application->qESKeyDRec) {
		lc = 0;
		rc = determinePinUseCounter(slot, sc->application->qESKeyDRec, &ptoken->pinUseCounter, &lc);

		if (rc < 0) {
			freeStarcosToken(ptoken);
			FUNC_FAILS(CKR_DEVICE_ERROR, "Error querying PIN key use counter");
		}

		if (lc == 0x23) {
			ptoken->pinChangeRequired = TRUE;
		}
	}

	if (ptoken->pinUseCounter != 1)
		ptoken->info.flags |= CKF_LOGIN_REQUIRED;

	rc = loadObjects(ptoken);

	if (rc < 0) {
		freeStarcosToken(ptoken);
		FUNC_FAILS(CKR_DEVICE_ERROR, "Error loading objects from token");
	}

	rc = checkPINStatus(slot, sc->application->pinref);

	if (rc < 0) {
		freeStarcosToken(ptoken);
		FUNC_FAILS(CKR_DEVICE_ERROR, "Error querying PIN status");
	}

	updatePinStatus(ptoken, rc);

	*token = ptoken;

	FUNC_RETURNS(CKR_OK);
}



static int getMechanismList(CK_MECHANISM_TYPE_PTR pMechanismList, CK_ULONG_PTR pulCount)
{
	int numberOfMechanisms;

	FUNC_CALLED();

	numberOfMechanisms = sizeof(p11MechanismList) / sizeof(p11MechanismList[0]);

	if (pMechanismList == NULL) {
		*pulCount = numberOfMechanisms;
		FUNC_RETURNS(CKR_OK);
	}

	if (*pulCount < numberOfMechanisms) {
		*pulCount = numberOfMechanisms;
		FUNC_FAILS(CKR_BUFFER_TOO_SMALL, "Buffer provided by caller too small");
	}

	memcpy(pMechanismList, p11MechanismList, sizeof(p11MechanismList));

	FUNC_RETURNS(CKR_OK);
}



static int getMechanismInfo(CK_MECHANISM_TYPE type, CK_MECHANISM_INFO_PTR pInfo)
{
	CK_RV rv = CKR_OK;

	FUNC_CALLED();

	switch (type) {
	case CKM_RSA_PKCS:
		pInfo->flags = CKF_SIGN|CKF_DECRYPT;
		break;
	case CKM_RSA_PKCS_OAEP:
		pInfo->flags = CKF_DECRYPT;
		break;
	case CKM_SHA1_RSA_PKCS:
	case CKM_SHA224_RSA_PKCS:
	case CKM_SHA256_RSA_PKCS:
	case CKM_SHA384_RSA_PKCS:
	case CKM_SHA512_RSA_PKCS:
	case CKM_SHA1_RSA_PKCS_PSS:
	case CKM_SHA224_RSA_PKCS_PSS:
	case CKM_SHA256_RSA_PKCS_PSS:
	case CKM_SHA384_RSA_PKCS_PSS:
	case CKM_SHA512_RSA_PKCS_PSS:
		pInfo->flags = CKF_SIGN;
		break;

	default:
		rv = CKR_MECHANISM_INVALID;
		break;
	}

	pInfo->ulMinKeySize = 2048;
	pInfo->ulMaxKeySize = 2048;
	FUNC_RETURNS(rv);
}



struct p11TokenDriver *getStarcosTokenDriver()
{
	static struct p11TokenDriver starcos_token = {
		"STARCOS",
		NULL,
		NULL,
		freeStarcosToken,
		getMechanismList,
		getMechanismInfo,
		login,
		logout,
		initpin,
		setpin
	};

	return &starcos_token;
}

