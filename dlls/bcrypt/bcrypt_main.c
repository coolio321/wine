/*
 * Copyright 2009 Henri Verbeet for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 *
 */

#include <stdarg.h>
#include <stdlib.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "ntsecapi.h"
#include "wincrypt.h"
#include "winternl.h"
#include "bcrypt.h"

#include "wine/debug.h"
#include "bcrypt_internal.h"

WINE_DEFAULT_DEBUG_CHANNEL(bcrypt);

static unixlib_handle_t bcrypt_handle;

#define UNIX_CALL( func, params ) __wine_unix_call( bcrypt_handle, unix_ ## func, params )


NTSTATUS WINAPI BCryptAddContextFunction( ULONG table, const WCHAR *ctx, ULONG iface, const WCHAR *func, ULONG pos )
{
    FIXME( "%#lx, %s, %#lx, %s, %lu: stub\n", table, debugstr_w(ctx), iface, debugstr_w(func), pos );
    return STATUS_SUCCESS;
}

NTSTATUS WINAPI BCryptAddContextFunctionProvider( ULONG table, const WCHAR *ctx, ULONG iface, const WCHAR *func,
                                                  const WCHAR *provider, ULONG pos )
{
    FIXME( "%#lx, %s, %#lx, %s, %s, %lu: stub\n", table, debugstr_w(ctx), iface, debugstr_w(func),
           debugstr_w(provider), pos );
    return STATUS_SUCCESS;
}

NTSTATUS WINAPI BCryptRemoveContextFunction( ULONG table, const WCHAR *ctx, ULONG iface, const WCHAR *func )
{
    FIXME( "%#lx, %s, %#lx, %s: stub\n", table, debugstr_w(ctx), iface, debugstr_w(func) );
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS WINAPI BCryptRemoveContextFunctionProvider( ULONG table, const WCHAR *ctx, ULONG iface, const WCHAR *func,
                                                     const WCHAR *provider )
{
    FIXME( "%#lx, %s, %#lx, %s, %s: stub\n", table, debugstr_w(ctx), iface, debugstr_w(func), debugstr_w(provider) );
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS WINAPI BCryptEnumContextFunctions( ULONG table, const WCHAR *ctx, ULONG iface, ULONG *buflen,
                                            CRYPT_CONTEXT_FUNCTIONS **buffer )
{
    FIXME( "%#lx, %s, %#lx, %p, %p\n", table, debugstr_w(ctx), iface, buflen, buffer );
    return STATUS_NOT_IMPLEMENTED;
}

void WINAPI BCryptFreeBuffer( void *buffer )
{
    free( buffer );
}

NTSTATUS WINAPI BCryptRegisterProvider( const WCHAR *provider, ULONG flags, CRYPT_PROVIDER_REG *reg )
{
    FIXME( "%s, %#lx, %p: stub\n", debugstr_w(provider), flags, reg );
    return STATUS_SUCCESS;
}

NTSTATUS WINAPI BCryptUnregisterProvider( const WCHAR *provider )
{
    FIXME( "%s: stub\n", debugstr_w(provider) );
    return STATUS_NOT_IMPLEMENTED;
}

#define MAX_HASH_OUTPUT_BYTES 64
#define MAX_HASH_BLOCK_BITS 1024

/* ordered by class, keep in sync with enum alg_id */
static const struct
{
    const WCHAR *name;
    ULONG        class;
    ULONG        object_length;
    ULONG        hash_length;
    ULONG        block_bits;
}
builtin_algorithms[] =
{
    {  BCRYPT_3DES_ALGORITHM,       BCRYPT_CIPHER_INTERFACE,                522,    0,    0 },
    {  BCRYPT_AES_ALGORITHM,        BCRYPT_CIPHER_INTERFACE,                654,    0,    0 },
    {  BCRYPT_SHA256_ALGORITHM,     BCRYPT_HASH_INTERFACE,                  286,   32,  512 },
    {  BCRYPT_SHA384_ALGORITHM,     BCRYPT_HASH_INTERFACE,                  382,   48, 1024 },
    {  BCRYPT_SHA512_ALGORITHM,     BCRYPT_HASH_INTERFACE,                  382,   64, 1024 },
    {  BCRYPT_SHA1_ALGORITHM,       BCRYPT_HASH_INTERFACE,                  278,   20,  512 },
    {  BCRYPT_MD5_ALGORITHM,        BCRYPT_HASH_INTERFACE,                  274,   16,  512 },
    {  BCRYPT_MD4_ALGORITHM,        BCRYPT_HASH_INTERFACE,                  270,   16,  512 },
    {  BCRYPT_MD2_ALGORITHM,        BCRYPT_HASH_INTERFACE,                  270,   16,  128 },
    {  BCRYPT_RSA_ALGORITHM,        BCRYPT_ASYMMETRIC_ENCRYPTION_INTERFACE, 0,      0,    0 },
    {  BCRYPT_ECDH_P256_ALGORITHM,  BCRYPT_SECRET_AGREEMENT_INTERFACE,      0,      0,    0 },
    {  BCRYPT_ECDH_P384_ALGORITHM,  BCRYPT_SECRET_AGREEMENT_INTERFACE,      0,      0,    0 },
    {  BCRYPT_RSA_SIGN_ALGORITHM,   BCRYPT_SIGNATURE_INTERFACE,             0,      0,    0 },
    {  BCRYPT_ECDSA_P256_ALGORITHM, BCRYPT_SIGNATURE_INTERFACE,             0,      0,    0 },
    {  BCRYPT_ECDSA_P384_ALGORITHM, BCRYPT_SIGNATURE_INTERFACE,             0,      0,    0 },
    {  BCRYPT_DSA_ALGORITHM,        BCRYPT_SIGNATURE_INTERFACE,             0,      0,    0 },
    {  BCRYPT_RNG_ALGORITHM,        BCRYPT_RNG_INTERFACE,                   0,      0,    0 },
};

static BOOL match_operation_type( ULONG type, ULONG class )
{
    if (!type) return TRUE;
    switch (class)
    {
    case BCRYPT_CIPHER_INTERFACE:                return type & BCRYPT_CIPHER_OPERATION;
    case BCRYPT_HASH_INTERFACE:                  return type & BCRYPT_HASH_OPERATION;
    case BCRYPT_ASYMMETRIC_ENCRYPTION_INTERFACE: return type & BCRYPT_ASYMMETRIC_ENCRYPTION_OPERATION;
    case BCRYPT_SECRET_AGREEMENT_INTERFACE:      return type & BCRYPT_SECRET_AGREEMENT_OPERATION;
    case BCRYPT_SIGNATURE_INTERFACE:             return type & BCRYPT_SIGNATURE_OPERATION;
    case BCRYPT_RNG_INTERFACE:                   return type & BCRYPT_RNG_OPERATION;
    default: break;
    }
    return FALSE;
}

NTSTATUS WINAPI BCryptEnumAlgorithms( ULONG type, ULONG *ret_count, BCRYPT_ALGORITHM_IDENTIFIER **ret_list, ULONG flags )
{
    static const ULONG supported = BCRYPT_CIPHER_OPERATION |\
                                   BCRYPT_HASH_OPERATION |\
                                   BCRYPT_ASYMMETRIC_ENCRYPTION_OPERATION |\
                                   BCRYPT_SECRET_AGREEMENT_OPERATION |\
                                   BCRYPT_SIGNATURE_OPERATION |\
                                   BCRYPT_RNG_OPERATION;
    BCRYPT_ALGORITHM_IDENTIFIER *list;
    ULONG i, j, count = 0;

    TRACE( "%#lx, %p, %p, %#lx\n", type, ret_count, ret_list, flags );

    if (!ret_count || !ret_list || (type & ~supported)) return STATUS_INVALID_PARAMETER;

    for (i = 0; i < ARRAY_SIZE( builtin_algorithms ); i++)
    {
        if (match_operation_type( type, builtin_algorithms[i].class )) count++;
    }

    if (!(list = malloc( count * sizeof(*list) ))) return STATUS_NO_MEMORY;

    for (i = 0, j = 0; i < ARRAY_SIZE( builtin_algorithms ); i++)
    {
        if (!match_operation_type( type, builtin_algorithms[i].class )) continue;
        list[j].pszName = (WCHAR *)builtin_algorithms[i].name;
        list[j].dwClass = builtin_algorithms[i].class;
        list[j].dwFlags = 0;
        j++;
    }

    *ret_count = count;
    *ret_list = list;
    return STATUS_SUCCESS;
}

NTSTATUS WINAPI BCryptGenRandom(BCRYPT_ALG_HANDLE handle, UCHAR *buffer, ULONG count, ULONG flags)
{
    const DWORD supported_flags = BCRYPT_USE_SYSTEM_PREFERRED_RNG;
    struct algorithm *algorithm = handle;

    TRACE("%p, %p, %lu, %#lx - semi-stub\n", handle, buffer, count, flags);

    if (!algorithm)
    {
        /* It's valid to call without an algorithm if BCRYPT_USE_SYSTEM_PREFERRED_RNG
         * is set. In this case the preferred system RNG is used.
         */
        if (!(flags & BCRYPT_USE_SYSTEM_PREFERRED_RNG))
            return STATUS_INVALID_HANDLE;
    }
    else if (algorithm->hdr.magic != MAGIC_ALG || algorithm->id != ALG_ID_RNG)
        return STATUS_INVALID_HANDLE;

    if (!buffer)
        return STATUS_INVALID_PARAMETER;

    if (flags & ~supported_flags)
        FIXME("unsupported flags %#lx\n", flags & ~supported_flags);

    if (algorithm)
        FIXME("ignoring selected algorithm\n");

    /* When zero bytes are requested the function returns success too. */
    if (!count)
        return STATUS_SUCCESS;

    if (algorithm || (flags & BCRYPT_USE_SYSTEM_PREFERRED_RNG))
    {
        if (RtlGenRandom(buffer, count))
            return STATUS_SUCCESS;
    }

    FIXME("called with unsupported parameters, returning error\n");
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS WINAPI BCryptOpenAlgorithmProvider( BCRYPT_ALG_HANDLE *handle, const WCHAR *id, const WCHAR *implementation,
                                             DWORD flags )
{
    const DWORD supported_flags = BCRYPT_ALG_HANDLE_HMAC_FLAG | BCRYPT_HASH_REUSABLE_FLAG;
    struct algorithm *alg;
    enum alg_id alg_id;
    ULONG i;

    TRACE( "%p, %s, %s, %#lx\n", handle, wine_dbgstr_w(id), wine_dbgstr_w(implementation), flags );

    if (!handle || !id) return STATUS_INVALID_PARAMETER;
    if (flags & ~supported_flags)
    {
        FIXME( "unsupported flags %#lx\n", flags & ~supported_flags );
        return STATUS_NOT_IMPLEMENTED;
    }

    for (i = 0; i < ARRAY_SIZE( builtin_algorithms ); i++)
    {
        if (!wcscmp( id, builtin_algorithms[i].name))
        {
            alg_id = i;
            break;
        }
    }
    if (i == ARRAY_SIZE( builtin_algorithms ))
    {
        FIXME( "algorithm %s not supported\n", debugstr_w(id) );
        return STATUS_NOT_IMPLEMENTED;
    }

    if (implementation && wcscmp( implementation, MS_PRIMITIVE_PROVIDER ))
    {
        FIXME( "implementation %s not supported\n", debugstr_w(implementation) );
        return STATUS_NOT_IMPLEMENTED;
    }

    if (!(alg = malloc( sizeof(*alg) ))) return STATUS_NO_MEMORY;
    alg->hdr.magic = MAGIC_ALG;
    alg->id        = alg_id;
    alg->mode      = MODE_ID_CBC;
    alg->flags     = flags;

    *handle = alg;
    return STATUS_SUCCESS;
}

NTSTATUS WINAPI BCryptCloseAlgorithmProvider( BCRYPT_ALG_HANDLE handle, DWORD flags )
{
    struct algorithm *alg = handle;

    TRACE( "%p, %#lx\n", handle, flags );

    if (!alg || alg->hdr.magic != MAGIC_ALG) return STATUS_INVALID_HANDLE;
    alg->hdr.magic = 0;
    free( alg );
    return STATUS_SUCCESS;
}

NTSTATUS WINAPI BCryptGetFipsAlgorithmMode(BOOLEAN *enabled)
{
    FIXME("%p - semi-stub\n", enabled);

    if (!enabled)
        return STATUS_INVALID_PARAMETER;

    *enabled = FALSE;
    return STATUS_SUCCESS;
}

struct hash_impl
{
    union
    {
        MD2_CTX md2;
        MD4_CTX md4;
        MD5_CTX md5;
        SHA_CTX sha1;
        SHA256_CTX sha256;
        SHA512_CTX sha512;
    } u;
};

static NTSTATUS hash_init( struct hash_impl *hash, enum alg_id alg_id )
{
    switch (alg_id)
    {
    case ALG_ID_MD2:
        md2_init( &hash->u.md2 );
        break;

    case ALG_ID_MD4:
        MD4Init( &hash->u.md4 );
        break;

    case ALG_ID_MD5:
        MD5Init( &hash->u.md5 );
        break;

    case ALG_ID_SHA1:
        A_SHAInit( &hash->u.sha1 );
        break;

    case ALG_ID_SHA256:
        sha256_init( &hash->u.sha256 );
        break;

    case ALG_ID_SHA384:
        sha384_init( &hash->u.sha512 );
        break;

    case ALG_ID_SHA512:
        sha512_init( &hash->u.sha512 );
        break;

    default:
        ERR( "unhandled id %u\n", alg_id );
        return STATUS_NOT_IMPLEMENTED;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS hash_update( struct hash_impl *hash, enum alg_id alg_id,
                             UCHAR *input, ULONG size )
{
    switch (alg_id)
    {
    case ALG_ID_MD2:
        md2_update( &hash->u.md2, input, size );
        break;

    case ALG_ID_MD4:
        MD4Update( &hash->u.md4, input, size );
        break;

    case ALG_ID_MD5:
        MD5Update( &hash->u.md5, input, size );
        break;

    case ALG_ID_SHA1:
        A_SHAUpdate( &hash->u.sha1, input, size );
        break;

    case ALG_ID_SHA256:
        sha256_update( &hash->u.sha256, input, size );
        break;

    case ALG_ID_SHA384:
        sha384_update( &hash->u.sha512, input, size );
        break;

    case ALG_ID_SHA512:
        sha512_update( &hash->u.sha512, input, size );
        break;

    default:
        ERR( "unhandled id %u\n", alg_id );
        return STATUS_NOT_IMPLEMENTED;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS hash_finish( struct hash_impl *hash, enum alg_id alg_id,
                             UCHAR *output, ULONG size )
{
    switch (alg_id)
    {
    case ALG_ID_MD2:
        md2_finalize( &hash->u.md2, output );
        break;

    case ALG_ID_MD4:
        MD4Final( &hash->u.md4 );
        memcpy( output, hash->u.md4.digest, 16 );
        break;

    case ALG_ID_MD5:
        MD5Final( &hash->u.md5 );
        memcpy( output, hash->u.md5.digest, 16 );
        break;

    case ALG_ID_SHA1:
        A_SHAFinal( &hash->u.sha1, (ULONG *)output );
        break;

    case ALG_ID_SHA256:
        sha256_finalize( &hash->u.sha256, output );
        break;

    case ALG_ID_SHA384:
        sha384_finalize( &hash->u.sha512, output );
        break;

    case ALG_ID_SHA512:
        sha512_finalize( &hash->u.sha512, output );
        break;

    default:
        ERR( "unhandled id %u\n", alg_id );
        return STATUS_NOT_IMPLEMENTED;
    }
    return STATUS_SUCCESS;
}

#define HASH_FLAG_HMAC      0x01
#define HASH_FLAG_REUSABLE  0x02
struct hash
{
    struct object     hdr;
    enum alg_id       alg_id;
    ULONG             flags;
    UCHAR            *secret;
    ULONG             secret_len;
    struct hash_impl  outer;
    struct hash_impl  inner;
};

#define BLOCK_LENGTH_3DES       8
#define BLOCK_LENGTH_AES        16

static NTSTATUS generic_alg_property( enum alg_id id, const WCHAR *prop, UCHAR *buf, ULONG size, ULONG *ret_size )
{
    if (!wcscmp( prop, BCRYPT_OBJECT_LENGTH ))
    {
        if (!builtin_algorithms[id].object_length)
            return STATUS_NOT_SUPPORTED;
        *ret_size = sizeof(ULONG);
        if (size < sizeof(ULONG))
            return STATUS_BUFFER_TOO_SMALL;
        if (buf)
            *(ULONG *)buf = builtin_algorithms[id].object_length;
        return STATUS_SUCCESS;
    }

    if (!wcscmp( prop, BCRYPT_HASH_LENGTH ))
    {
        if (!builtin_algorithms[id].hash_length)
            return STATUS_NOT_SUPPORTED;
        *ret_size = sizeof(ULONG);
        if (size < sizeof(ULONG))
            return STATUS_BUFFER_TOO_SMALL;
        if(buf)
            *(ULONG*)buf = builtin_algorithms[id].hash_length;
        return STATUS_SUCCESS;
    }

    if (!wcscmp( prop, BCRYPT_ALGORITHM_NAME ))
    {
        *ret_size = (lstrlenW(builtin_algorithms[id].name) + 1) * sizeof(WCHAR);
        if (size < *ret_size)
            return STATUS_BUFFER_TOO_SMALL;
        if(buf)
            memcpy(buf, builtin_algorithms[id].name, *ret_size);
        return STATUS_SUCCESS;
    }

    return STATUS_NOT_IMPLEMENTED;
}

static NTSTATUS get_3des_property( enum mode_id mode, const WCHAR *prop, UCHAR *buf, ULONG size, ULONG *ret_size )
{
    if (!wcscmp( prop, BCRYPT_BLOCK_LENGTH ))
    {
        *ret_size = sizeof(ULONG);
        if (size < sizeof(ULONG)) return STATUS_BUFFER_TOO_SMALL;
        if (buf) *(ULONG *)buf = BLOCK_LENGTH_3DES;
        return STATUS_SUCCESS;
    }
    if (!wcscmp( prop, BCRYPT_CHAINING_MODE ))
    {
        const WCHAR *str;
        switch (mode)
        {
        case MODE_ID_CBC: str = BCRYPT_CHAIN_MODE_CBC; break;
        default: return STATUS_NOT_IMPLEMENTED;
        }

        *ret_size = 64;
        if (size < *ret_size) return STATUS_BUFFER_TOO_SMALL;
        memcpy( buf, str, (lstrlenW(str) + 1) * sizeof(WCHAR) );
        return STATUS_SUCCESS;
    }
    if (!wcscmp( prop, BCRYPT_KEY_LENGTHS ))
    {
        BCRYPT_KEY_LENGTHS_STRUCT *key_lengths = (void *)buf;
        *ret_size = sizeof(*key_lengths);
        if (key_lengths && size < *ret_size) return STATUS_BUFFER_TOO_SMALL;
        if (key_lengths)
        {
            key_lengths->dwMinLength = 192;
            key_lengths->dwMaxLength = 192;
            key_lengths->dwIncrement = 0;
        }
        return STATUS_SUCCESS;
    }
    FIXME( "unsupported property %s\n", debugstr_w(prop) );
    return STATUS_NOT_IMPLEMENTED;
}

static NTSTATUS get_aes_property( enum mode_id mode, const WCHAR *prop, UCHAR *buf, ULONG size, ULONG *ret_size )
{
    if (!wcscmp( prop, BCRYPT_BLOCK_LENGTH ))
    {
        *ret_size = sizeof(ULONG);
        if (size < sizeof(ULONG)) return STATUS_BUFFER_TOO_SMALL;
        if (buf) *(ULONG *)buf = BLOCK_LENGTH_AES;
        return STATUS_SUCCESS;
    }
    if (!wcscmp( prop, BCRYPT_CHAINING_MODE ))
    {
        const WCHAR *str;
        switch (mode)
        {
        case MODE_ID_ECB: str = BCRYPT_CHAIN_MODE_ECB; break;
        case MODE_ID_CBC: str = BCRYPT_CHAIN_MODE_CBC; break;
        case MODE_ID_GCM: str = BCRYPT_CHAIN_MODE_GCM; break;
        case MODE_ID_CFB: str = BCRYPT_CHAIN_MODE_CFB; break;
        default: return STATUS_NOT_IMPLEMENTED;
        }

        *ret_size = 64;
        if (size < *ret_size) return STATUS_BUFFER_TOO_SMALL;
        memcpy( buf, str, (lstrlenW(str) + 1) * sizeof(WCHAR) );
        return STATUS_SUCCESS;
    }
    if (!wcscmp( prop, BCRYPT_KEY_LENGTHS ))
    {
        BCRYPT_KEY_LENGTHS_STRUCT *key_lengths = (void *)buf;
        *ret_size = sizeof(*key_lengths);
        if (key_lengths && size < *ret_size) return STATUS_BUFFER_TOO_SMALL;
        if (key_lengths)
        {
            key_lengths->dwMinLength = 128;
            key_lengths->dwMaxLength = 256;
            key_lengths->dwIncrement = 64;
        }
        return STATUS_SUCCESS;
    }
    if (!wcscmp( prop, BCRYPT_AUTH_TAG_LENGTH ))
    {
        BCRYPT_AUTH_TAG_LENGTHS_STRUCT *tag_length = (void *)buf;
        if (mode != MODE_ID_GCM) return STATUS_NOT_SUPPORTED;
        *ret_size = sizeof(*tag_length);
        if (tag_length && size < *ret_size) return STATUS_BUFFER_TOO_SMALL;
        if (tag_length)
        {
            tag_length->dwMinLength = 12;
            tag_length->dwMaxLength = 16;
            tag_length->dwIncrement =  1;
        }
        return STATUS_SUCCESS;
    }

    FIXME( "unsupported property %s\n", debugstr_w(prop) );
    return STATUS_NOT_IMPLEMENTED;
}

static NTSTATUS get_rsa_property( enum mode_id mode, const WCHAR *prop, UCHAR *buf, ULONG size, ULONG *ret_size )
{
    if (!wcscmp( prop, BCRYPT_PADDING_SCHEMES ))
    {
        *ret_size = sizeof(ULONG);
        if (size < sizeof(ULONG)) return STATUS_BUFFER_TOO_SMALL;
        if (buf) *(ULONG *)buf = BCRYPT_SUPPORTED_PAD_PKCS1_SIG;
        return STATUS_SUCCESS;
    }

    FIXME( "unsupported property %s\n", debugstr_w(prop) );
    return STATUS_NOT_IMPLEMENTED;
}

static NTSTATUS get_dsa_property( enum mode_id mode, const WCHAR *prop, UCHAR *buf, ULONG size, ULONG *ret_size )
{
    if (!wcscmp( prop, BCRYPT_PADDING_SCHEMES )) return STATUS_NOT_SUPPORTED;
    FIXME( "unsupported property %s\n", debugstr_w(prop) );
    return STATUS_NOT_IMPLEMENTED;
}

static NTSTATUS get_alg_property( const struct algorithm *alg, const WCHAR *prop,
                                  UCHAR *buf, ULONG size, ULONG *ret_size )
{
    NTSTATUS status;

    status = generic_alg_property( alg->id, prop, buf, size, ret_size );
    if (status != STATUS_NOT_IMPLEMENTED)
        return status;

    switch (alg->id)
    {
    case ALG_ID_3DES:
        return get_3des_property( alg->mode, prop, buf, size, ret_size );

    case ALG_ID_AES:
        return get_aes_property( alg->mode, prop, buf, size, ret_size );

    case ALG_ID_RSA:
        return get_rsa_property( alg->mode, prop, buf, size, ret_size );

    case ALG_ID_DSA:
        return get_dsa_property( alg->mode, prop, buf, size, ret_size );

    default:
        break;
    }

    FIXME( "unsupported property %s algorithm %u\n", debugstr_w(prop), alg->id );
    return STATUS_NOT_IMPLEMENTED;
}

static NTSTATUS set_alg_property( struct algorithm *alg, const WCHAR *prop, UCHAR *value, ULONG size, ULONG flags )
{
    switch (alg->id)
    {
    case ALG_ID_3DES:
        if (!wcscmp( prop, BCRYPT_CHAINING_MODE ))
        {
            if (!wcscmp( (WCHAR *)value, BCRYPT_CHAIN_MODE_CBC ))
            {
                alg->mode = MODE_ID_CBC;
                return STATUS_SUCCESS;
            }
            else
            {
                FIXME( "unsupported mode %s\n", debugstr_w((WCHAR *)value) );
                return STATUS_NOT_SUPPORTED;
            }
        }
        FIXME( "unsupported 3des algorithm property %s\n", debugstr_w(prop) );
        return STATUS_NOT_IMPLEMENTED;

    case ALG_ID_AES:
        if (!wcscmp( prop, BCRYPT_CHAINING_MODE ))
        {
            if (!wcscmp( (WCHAR *)value, BCRYPT_CHAIN_MODE_ECB ))
            {
                alg->mode = MODE_ID_ECB;
                return STATUS_SUCCESS;
            }
            else if (!wcscmp( (WCHAR *)value, BCRYPT_CHAIN_MODE_CBC ))
            {
                alg->mode = MODE_ID_CBC;
                return STATUS_SUCCESS;
            }
            else if (!wcscmp( (WCHAR *)value, BCRYPT_CHAIN_MODE_GCM ))
            {
                alg->mode = MODE_ID_GCM;
                return STATUS_SUCCESS;
            }
            else if (!wcscmp( (WCHAR *)value, BCRYPT_CHAIN_MODE_CFB ))
            {
                alg->mode = MODE_ID_CFB;
                return STATUS_SUCCESS;
            }
            else
            {
                FIXME( "unsupported mode %s\n", debugstr_w((WCHAR *)value) );
                return STATUS_NOT_IMPLEMENTED;
            }
        }
        FIXME( "unsupported aes algorithm property %s\n", debugstr_w(prop) );
        return STATUS_NOT_IMPLEMENTED;

    default:
        FIXME( "unsupported algorithm %u\n", alg->id );
        return STATUS_NOT_IMPLEMENTED;
    }
}

static NTSTATUS set_key_property( struct key *key, const WCHAR *prop, UCHAR *value, ULONG size, ULONG flags )
{
    if (!wcscmp( prop, BCRYPT_CHAINING_MODE ))
    {
        if (!wcscmp( (WCHAR *)value, BCRYPT_CHAIN_MODE_ECB ))
        {
            key->u.s.mode = MODE_ID_ECB;
            return STATUS_SUCCESS;
        }
        else if (!wcscmp( (WCHAR *)value, BCRYPT_CHAIN_MODE_CBC ))
        {
            key->u.s.mode = MODE_ID_CBC;
            return STATUS_SUCCESS;
        }
        else if (!wcscmp( (WCHAR *)value, BCRYPT_CHAIN_MODE_GCM ))
        {
            key->u.s.mode = MODE_ID_GCM;
            return STATUS_SUCCESS;
        }
        else
        {
            FIXME( "unsupported mode %s\n", debugstr_w((WCHAR *)value) );
            return STATUS_NOT_IMPLEMENTED;
        }
    }
    else if (!wcscmp( prop, BCRYPT_KEY_LENGTH ))
    {
        if (size < sizeof(DWORD)) return STATUS_INVALID_PARAMETER;
        key->u.a.bitlen = *(DWORD*)value;
        return STATUS_SUCCESS;
    }

    FIXME( "unsupported key property %s\n", debugstr_w(prop) );
    return STATUS_NOT_IMPLEMENTED;
}

static NTSTATUS get_hash_property( const struct hash *hash, const WCHAR *prop, UCHAR *buf, ULONG size, ULONG *ret_size )
{
    NTSTATUS status;

    status = generic_alg_property( hash->alg_id, prop, buf, size, ret_size );
    if (status == STATUS_NOT_IMPLEMENTED)
        FIXME( "unsupported property %s\n", debugstr_w(prop) );
    return status;
}

static NTSTATUS get_key_property( const struct key *key, const WCHAR *prop, UCHAR *buf, ULONG size, ULONG *ret_size )
{
    switch (key->alg_id)
    {
    case ALG_ID_3DES:
        return get_3des_property( key->u.s.mode, prop, buf, size, ret_size );

    case ALG_ID_AES:
        if (!wcscmp( prop, BCRYPT_AUTH_TAG_LENGTH )) return STATUS_NOT_SUPPORTED;
        return get_aes_property( key->u.s.mode, prop, buf, size, ret_size );

    default:
        FIXME( "unsupported algorithm %u\n", key->alg_id );
        return STATUS_NOT_IMPLEMENTED;
    }
}

NTSTATUS WINAPI BCryptGetProperty( BCRYPT_HANDLE handle, const WCHAR *prop, UCHAR *buffer, ULONG count, ULONG *res,
                                   ULONG flags )
{
    struct object *object = handle;

    TRACE( "%p, %s, %p, %lu, %p, %#lx\n", handle, wine_dbgstr_w(prop), buffer, count, res, flags );

    if (!object) return STATUS_INVALID_HANDLE;
    if (!prop || !res) return STATUS_INVALID_PARAMETER;

    switch (object->magic)
    {
    case MAGIC_ALG:
    {
        const struct algorithm *alg = (const struct algorithm *)object;
        return get_alg_property( alg, prop, buffer, count, res );
    }
    case MAGIC_KEY:
    {
        const struct key *key = (const struct key *)object;
        return get_key_property( key, prop, buffer, count, res );
    }
    case MAGIC_HASH:
    {
        const struct hash *hash = (const struct hash *)object;
        return get_hash_property( hash, prop, buffer, count, res );
    }
    default:
        WARN( "unknown magic %#lx\n", object->magic );
        return STATUS_INVALID_HANDLE;
    }
}

static NTSTATUS hash_prepare( struct hash *hash )
{
    UCHAR buffer[MAX_HASH_BLOCK_BITS / 8] = {0};
    int block_bytes, i;
    NTSTATUS status;

    /* initialize hash */
    if ((status = hash_init( &hash->inner, hash->alg_id ))) return status;
    if (!(hash->flags & HASH_FLAG_HMAC)) return STATUS_SUCCESS;

    /* initialize hmac */
    if ((status = hash_init( &hash->outer, hash->alg_id ))) return status;
    block_bytes = builtin_algorithms[hash->alg_id].block_bits / 8;
    if (hash->secret_len > block_bytes)
    {
        struct hash_impl temp;
        if ((status = hash_init( &temp, hash->alg_id ))) return status;
        if ((status = hash_update( &temp, hash->alg_id, hash->secret, hash->secret_len ))) return status;
        if ((status = hash_finish( &temp, hash->alg_id, buffer,
                                   builtin_algorithms[hash->alg_id].hash_length ))) return status;
    }
    else memcpy( buffer, hash->secret, hash->secret_len );

    for (i = 0; i < block_bytes; i++) buffer[i] ^= 0x5c;
    if ((status = hash_update( &hash->outer, hash->alg_id, buffer, block_bytes ))) return status;
    for (i = 0; i < block_bytes; i++) buffer[i] ^= (0x5c ^ 0x36);
    return hash_update( &hash->inner, hash->alg_id, buffer, block_bytes );
}

static NTSTATUS hash_create( const struct algorithm *alg, UCHAR *secret, ULONG secret_len, ULONG flags,
                             struct hash **ret_hash )
{
    struct hash *hash;
    NTSTATUS status;

    if (!(hash = calloc( 1, sizeof(*hash) ))) return STATUS_NO_MEMORY;
    hash->hdr.magic = MAGIC_HASH;
    hash->alg_id    = alg->id;
    if (alg->flags & BCRYPT_ALG_HANDLE_HMAC_FLAG) hash->flags = HASH_FLAG_HMAC;
    if ((alg->flags & BCRYPT_HASH_REUSABLE_FLAG) || (flags & BCRYPT_HASH_REUSABLE_FLAG))
        hash->flags |= HASH_FLAG_REUSABLE;

    if (secret_len && !(hash->secret = malloc( secret_len )))
    {
        free( hash );
        return STATUS_NO_MEMORY;
    }
    memcpy( hash->secret, secret, secret_len );
    hash->secret_len = secret_len;

    if ((status = hash_prepare( hash )))
    {
        free( hash->secret );
        free( hash );
        return status;
    }

    *ret_hash = hash;
    return STATUS_SUCCESS;
}

NTSTATUS WINAPI BCryptCreateHash( BCRYPT_ALG_HANDLE algorithm, BCRYPT_HASH_HANDLE *handle, UCHAR *object,
                                  ULONG object_len, UCHAR *secret, ULONG secret_len, ULONG flags )
{
    struct algorithm *alg = algorithm;
    struct hash *hash;
    NTSTATUS status;

    TRACE( "%p, %p, %p, %lu, %p, %lu, %#lx\n", algorithm, handle, object, object_len, secret, secret_len, flags );
    if (flags & ~BCRYPT_HASH_REUSABLE_FLAG)
    {
        FIXME( "unimplemented flags %#lx\n", flags );
        return STATUS_NOT_IMPLEMENTED;
    }

    if (!alg || alg->hdr.magic != MAGIC_ALG) return STATUS_INVALID_HANDLE;
    if (object) FIXME( "ignoring object buffer\n" );

    if ((status = hash_create( alg, secret, secret_len, flags, &hash ))) return status;
    *handle = hash;
    return STATUS_SUCCESS;
}

NTSTATUS WINAPI BCryptDuplicateHash( BCRYPT_HASH_HANDLE handle, BCRYPT_HASH_HANDLE *handle_copy,
                                     UCHAR *object, ULONG objectlen, ULONG flags )
{
    struct hash *hash_orig = handle;
    struct hash *hash_copy;

    TRACE( "%p, %p, %p, %lu, %#lx\n", handle, handle_copy, object, objectlen, flags );

    if (!hash_orig || hash_orig->hdr.magic != MAGIC_HASH) return STATUS_INVALID_HANDLE;
    if (!handle_copy) return STATUS_INVALID_PARAMETER;
    if (object) FIXME( "ignoring object buffer\n" );

    if (!(hash_copy = malloc( sizeof(*hash_copy) ))) return STATUS_NO_MEMORY;

    memcpy( hash_copy, hash_orig, sizeof(*hash_orig) );
    if (hash_orig->secret && !(hash_copy->secret = malloc( hash_orig->secret_len )))
    {
        free( hash_copy );
        return STATUS_NO_MEMORY;
    }
    memcpy( hash_copy->secret, hash_orig->secret, hash_orig->secret_len );

    *handle_copy = hash_copy;
    return STATUS_SUCCESS;
}

static void hash_destroy( struct hash *hash )
{
    if (!hash) return;
    hash->hdr.magic = 0;
    free( hash->secret );
    free( hash );
}

NTSTATUS WINAPI BCryptDestroyHash( BCRYPT_HASH_HANDLE handle )
{
    struct hash *hash = handle;

    TRACE( "%p\n", handle );

    if (!hash || hash->hdr.magic != MAGIC_HASH) return STATUS_INVALID_PARAMETER;
    hash_destroy( hash );
    return STATUS_SUCCESS;
}

NTSTATUS WINAPI BCryptHashData( BCRYPT_HASH_HANDLE handle, UCHAR *input, ULONG size, ULONG flags )
{
    struct hash *hash = handle;

    TRACE( "%p, %p, %lu, %#lx\n", handle, input, size, flags );

    if (!hash || hash->hdr.magic != MAGIC_HASH) return STATUS_INVALID_HANDLE;
    if (!input) return STATUS_SUCCESS;

    return hash_update( &hash->inner, hash->alg_id, input, size );
}

static NTSTATUS hash_finalize( struct hash *hash, UCHAR *output, ULONG size )
{
    UCHAR buffer[MAX_HASH_OUTPUT_BYTES];
    int hash_length;
    NTSTATUS status;

    if (!(hash->flags & HASH_FLAG_HMAC))
    {
        if ((status = hash_finish( &hash->inner, hash->alg_id, output, size ))) return status;
        if (hash->flags & HASH_FLAG_REUSABLE) return hash_prepare( hash );
        return STATUS_SUCCESS;
    }

    hash_length = builtin_algorithms[hash->alg_id].hash_length;
    if ((status = hash_finish( &hash->inner, hash->alg_id, buffer, hash_length ))) return status;
    if ((status = hash_update( &hash->outer, hash->alg_id, buffer, hash_length ))) return status;
    if ((status = hash_finish( &hash->outer, hash->alg_id, output, size ))) return status;

    if (hash->flags & HASH_FLAG_REUSABLE) return hash_prepare( hash );
    return STATUS_SUCCESS;
}

NTSTATUS WINAPI BCryptFinishHash( BCRYPT_HASH_HANDLE handle, UCHAR *output, ULONG size, ULONG flags )
{
    struct hash *hash = handle;

    TRACE( "%p, %p, %lu, %#lx\n", handle, output, size, flags );

    if (!hash || hash->hdr.magic != MAGIC_HASH) return STATUS_INVALID_HANDLE;
    if (!output) return STATUS_INVALID_PARAMETER;

    return hash_finalize( hash, output, size );
}

NTSTATUS WINAPI BCryptHash( BCRYPT_ALG_HANDLE algorithm, UCHAR *secret, ULONG secret_len,
                            UCHAR *input, ULONG input_len, UCHAR *output, ULONG output_len )
{
    struct algorithm *alg = algorithm;
    struct hash *hash;
    NTSTATUS status;

    TRACE( "%p, %p, %lu, %p, %lu, %p, %lu\n", algorithm, secret, secret_len, input, input_len, output, output_len );

    if (!alg || alg->hdr.magic != MAGIC_ALG) return STATUS_INVALID_HANDLE;

    if ((status = hash_create( alg, secret, secret_len, 0, &hash ))) return status;
    if ((status = hash_update( &hash->inner, hash->alg_id, input, input_len )))
    {
        hash_destroy( hash );
        return status;
    }
    status = hash_finalize( hash, output, output_len );
    hash_destroy( hash );
    return status;
}

static NTSTATUS key_asymmetric_create( enum alg_id alg_id, ULONG bitlen, struct key **ret_key )
{
    struct key *key;

    if (!bcrypt_handle)
    {
        ERR( "no encryption support\n" );
        return STATUS_NOT_IMPLEMENTED;
    }

    if (!(key = calloc( 1, sizeof(*key) ))) return STATUS_NO_MEMORY;
    key->hdr.magic  = MAGIC_KEY;
    key->alg_id     = alg_id;
    key->u.a.bitlen = bitlen;

    *ret_key = key;
    return STATUS_SUCCESS;
}

static BOOL key_is_symmetric( struct key *key )
{
    return builtin_algorithms[key->alg_id].class == BCRYPT_CIPHER_INTERFACE;
}

static BOOL is_zero_vector( const UCHAR *vector, ULONG len )
{
    ULONG i;
    if (!vector) return FALSE;
    for (i = 0; i < len; i++) if (vector[i]) return FALSE;
    return TRUE;
}

static BOOL is_equal_vector( const UCHAR *vector, ULONG len, const UCHAR *vector2, ULONG len2 )
{
    if (!vector && !vector2) return TRUE;
    if (len != len2) return FALSE;
    return !memcmp( vector, vector2, len );
}

static NTSTATUS key_symmetric_set_vector( struct key *key, UCHAR *vector, ULONG vector_len )
{
    BOOL needs_reset = (!is_zero_vector( vector, vector_len ) ||
                        !is_equal_vector( key->u.s.vector, key->u.s.vector_len, vector, vector_len ));

    free( key->u.s.vector );
    key->u.s.vector = NULL;
    key->u.s.vector_len = 0;
    if (vector)
    {
        if (!(key->u.s.vector = malloc( vector_len ))) return STATUS_NO_MEMORY;
        memcpy( key->u.s.vector, vector, vector_len );
        key->u.s.vector_len = vector_len;
    }
    if (needs_reset) UNIX_CALL( key_symmetric_vector_reset, key );
    return STATUS_SUCCESS;
}

static NTSTATUS key_import( BCRYPT_ALG_HANDLE algorithm, const WCHAR *type, BCRYPT_KEY_HANDLE *key, UCHAR *object,
                            ULONG object_len, UCHAR *input, ULONG input_len )
{
    ULONG len;

    if (!wcscmp( type, BCRYPT_KEY_DATA_BLOB ))
    {
        BCRYPT_KEY_DATA_BLOB_HEADER *header = (BCRYPT_KEY_DATA_BLOB_HEADER *)input;

        if (input_len < sizeof(BCRYPT_KEY_DATA_BLOB_HEADER)) return STATUS_BUFFER_TOO_SMALL;
        if (header->dwMagic != BCRYPT_KEY_DATA_BLOB_MAGIC) return STATUS_INVALID_PARAMETER;
        if (header->dwVersion != BCRYPT_KEY_DATA_BLOB_VERSION1)
        {
            FIXME( "unknown key data blob version %lu\n", header->dwVersion );
            return STATUS_INVALID_PARAMETER;
        }
        len = header->cbKeyData;
        if (len + sizeof(BCRYPT_KEY_DATA_BLOB_HEADER) > input_len) return STATUS_INVALID_PARAMETER;

        return BCryptGenerateSymmetricKey( algorithm, key, object, object_len, (UCHAR *)&header[1], len, 0 );
    }
    else if (!wcscmp( type, BCRYPT_OPAQUE_KEY_BLOB ))
    {
        if (input_len < sizeof(len)) return STATUS_BUFFER_TOO_SMALL;
        len = *(ULONG *)input;
        if (len + sizeof(len) > input_len) return STATUS_INVALID_PARAMETER;

        return BCryptGenerateSymmetricKey( algorithm, key, object, object_len, input + sizeof(len), len, 0 );
    }

    FIXME( "unsupported key type %s\n", debugstr_w(type) );
    return STATUS_NOT_IMPLEMENTED;
}

static NTSTATUS key_export( struct key *key, const WCHAR *type, UCHAR *output, ULONG output_len, ULONG *size )
{
    struct key_asymmetric_export_params params;

    if (!wcscmp( type, BCRYPT_KEY_DATA_BLOB ))
    {
        BCRYPT_KEY_DATA_BLOB_HEADER *header = (BCRYPT_KEY_DATA_BLOB_HEADER *)output;
        ULONG req_size = sizeof(BCRYPT_KEY_DATA_BLOB_HEADER) + key->u.s.secret_len;

        *size = req_size;
        if (output_len < req_size) return STATUS_BUFFER_TOO_SMALL;
        if (output)
        {
            header->dwMagic   = BCRYPT_KEY_DATA_BLOB_MAGIC;
            header->dwVersion = BCRYPT_KEY_DATA_BLOB_VERSION1;
            header->cbKeyData = key->u.s.secret_len;
            memcpy( &header[1], key->u.s.secret, key->u.s.secret_len );
        }
        return STATUS_SUCCESS;
    }
    else if (!wcscmp( type, BCRYPT_OPAQUE_KEY_BLOB ))
    {
        ULONG len, req_size = sizeof(len) + key->u.s.secret_len;

        *size = req_size;
        if (output_len < req_size) return STATUS_BUFFER_TOO_SMALL;
        if (output)
        {
            *(ULONG *)output = key->u.s.secret_len;
            memcpy( output + sizeof(len), key->u.s.secret, key->u.s.secret_len );
        }
        return STATUS_SUCCESS;
    }
    else if (!wcscmp( type, BCRYPT_DSA_PRIVATE_BLOB ) || !wcscmp( type, LEGACY_DSA_V2_PRIVATE_BLOB ) ||
             !wcscmp( type, BCRYPT_ECCPRIVATE_BLOB ))
    {
        params.key     = key;
        params.flags   = 0;
        params.buf     = output;
        params.len     = output_len;
        params.ret_len = size;
        return UNIX_CALL( key_asymmetric_export, &params );
    }
    else if (!wcscmp( type, BCRYPT_RSAPRIVATE_BLOB ) || !wcscmp( type, BCRYPT_RSAFULLPRIVATE_BLOB ))
    {
        params.key     = key;
        params.flags   = (wcscmp( type, BCRYPT_RSAPRIVATE_BLOB )) ? KEY_EXPORT_FLAG_RSA_FULL : 0;
        params.buf     = output;
        params.len     = output_len;
        params.ret_len = size;
        return UNIX_CALL( key_asymmetric_export, &params );
    }
    else if (!wcscmp( type, BCRYPT_DSA_PUBLIC_BLOB ) || !wcscmp( type, LEGACY_DSA_V2_PUBLIC_BLOB ) ||
             !wcscmp( type, BCRYPT_ECCPUBLIC_BLOB ) || !wcscmp( type, BCRYPT_RSAPUBLIC_BLOB ))
    {
        params.key     = key;
        params.flags   = KEY_EXPORT_FLAG_PUBLIC;
        params.buf     = output;
        params.len     = output_len;
        params.ret_len = size;
        return UNIX_CALL( key_asymmetric_export, &params );
    }

    FIXME( "unsupported key type %s\n", debugstr_w(type) );
    return STATUS_NOT_IMPLEMENTED;
}

static NTSTATUS key_symmetric_encrypt( struct key *key,  UCHAR *input, ULONG input_len, void *padding, UCHAR *iv,
                                       ULONG iv_len, UCHAR *output, ULONG output_len, ULONG *ret_len, ULONG flags )
{
    struct key_symmetric_set_auth_data_params auth_params;
    struct key_symmetric_encrypt_params encrypt_params;
    struct key_symmetric_get_tag_params tag_params;
    ULONG bytes_left = input_len;
    UCHAR *buf;
    NTSTATUS status;

    if (key->u.s.mode == MODE_ID_GCM)
    {
        BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO *auth_info = padding;

        if (!auth_info) return STATUS_INVALID_PARAMETER;
        if (!auth_info->pbNonce) return STATUS_INVALID_PARAMETER;
        if (!auth_info->pbTag) return STATUS_INVALID_PARAMETER;
        if (auth_info->cbTag < 12 || auth_info->cbTag > 16) return STATUS_INVALID_PARAMETER;
        if (auth_info->dwFlags & BCRYPT_AUTH_MODE_CHAIN_CALLS_FLAG)
            FIXME( "call chaining not implemented\n" );

        if ((status = key_symmetric_set_vector( key, auth_info->pbNonce, auth_info->cbNonce )))
            return status;

        *ret_len = input_len;
        if (flags & BCRYPT_BLOCK_PADDING) return STATUS_INVALID_PARAMETER;
        if (input && !output) return STATUS_SUCCESS;
        if (output_len < *ret_len) return STATUS_BUFFER_TOO_SMALL;

        auth_params.key = key;
        auth_params.auth_data = auth_info->pbAuthData;
        auth_params.len = auth_info->cbAuthData;
        if ((status = UNIX_CALL( key_symmetric_set_auth_data, &auth_params ))) return status;

        encrypt_params.key = key;
        encrypt_params.input = input;
        encrypt_params.input_len = input_len;
        encrypt_params.output = output;
        encrypt_params.output_len = output_len;
        if ((status = UNIX_CALL( key_symmetric_encrypt, &encrypt_params ))) return status;

        tag_params.key = key;
        tag_params.tag = auth_info->pbTag;
        tag_params.len = auth_info->cbTag;
        return UNIX_CALL( key_symmetric_get_tag, &tag_params );
    }

    *ret_len = input_len;

    if (flags & BCRYPT_BLOCK_PADDING)
        *ret_len = (input_len + key->u.s.block_size) & ~(key->u.s.block_size - 1);
    else if (input_len & (key->u.s.block_size - 1))
        return STATUS_INVALID_BUFFER_SIZE;

    if (!output) return STATUS_SUCCESS;
    if (output_len < *ret_len) return STATUS_BUFFER_TOO_SMALL;
    if (key->u.s.mode == MODE_ID_ECB && iv) return STATUS_INVALID_PARAMETER;
    if ((status = key_symmetric_set_vector( key, iv, iv_len ))) return status;

    encrypt_params.key = key;
    encrypt_params.input = input;
    encrypt_params.input_len = key->u.s.block_size;
    encrypt_params.output = output;
    encrypt_params.output_len = key->u.s.block_size;
    while (bytes_left >= key->u.s.block_size)
    {
        if ((status = UNIX_CALL( key_symmetric_encrypt, &encrypt_params )))
            return status;
        if (key->u.s.mode == MODE_ID_ECB && (status = key_symmetric_set_vector( key, NULL, 0 )))
            return status;
        bytes_left -= key->u.s.block_size;
        encrypt_params.input += key->u.s.block_size;
        encrypt_params.output += key->u.s.block_size;
    }

    if (flags & BCRYPT_BLOCK_PADDING)
    {
        if (!(buf = malloc( key->u.s.block_size ))) return STATUS_NO_MEMORY;
        memcpy( buf, encrypt_params.input, bytes_left );
        memset( buf + bytes_left, key->u.s.block_size - bytes_left, key->u.s.block_size - bytes_left );
        encrypt_params.input = buf;
        status = UNIX_CALL( key_symmetric_encrypt, &encrypt_params );
        free( buf );
    }

    return status;
}

static NTSTATUS key_symmetric_decrypt( struct key *key, UCHAR *input, ULONG input_len, void *padding, UCHAR *iv,
                                       ULONG iv_len, UCHAR *output, ULONG output_len, ULONG *ret_len, ULONG flags )
{
    struct key_symmetric_set_auth_data_params auth_params;
    struct key_symmetric_decrypt_params decrypt_params;
    struct key_symmetric_get_tag_params tag_params;
    ULONG bytes_left = input_len;
    NTSTATUS status;

    if (key->u.s.mode == MODE_ID_GCM)
    {
        BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO *auth_info = padding;
        UCHAR tag[16];

        if (!auth_info) return STATUS_INVALID_PARAMETER;
        if (!auth_info->pbNonce) return STATUS_INVALID_PARAMETER;
        if (!auth_info->pbTag) return STATUS_INVALID_PARAMETER;
        if (auth_info->cbTag < 12 || auth_info->cbTag > 16) return STATUS_INVALID_PARAMETER;

        if ((status = key_symmetric_set_vector( key, auth_info->pbNonce, auth_info->cbNonce )))
            return status;

        *ret_len = input_len;
        if (flags & BCRYPT_BLOCK_PADDING) return STATUS_INVALID_PARAMETER;
        if (!output) return STATUS_SUCCESS;
        if (output_len < *ret_len) return STATUS_BUFFER_TOO_SMALL;

        auth_params.key = key;
        auth_params.auth_data = auth_info->pbAuthData;
        auth_params.len = auth_info->cbAuthData;
        if ((status = UNIX_CALL( key_symmetric_set_auth_data, &auth_params ))) return status;

        decrypt_params.key = key;
        decrypt_params.input = input;
        decrypt_params.input_len = input_len;
        decrypt_params.output = output;
        decrypt_params.output_len = output_len;
        if ((status = UNIX_CALL( key_symmetric_decrypt, &decrypt_params ))) return status;

        tag_params.key = key;
        tag_params.tag = tag;
        tag_params.len = sizeof(tag);
        if ((status = UNIX_CALL( key_symmetric_get_tag, &tag_params ))) return status;
        if (memcmp( tag, auth_info->pbTag, auth_info->cbTag )) return STATUS_AUTH_TAG_MISMATCH;

        return STATUS_SUCCESS;
    }

    *ret_len = input_len;

    if (input_len & (key->u.s.block_size - 1)) return STATUS_INVALID_BUFFER_SIZE;
    if (!output) return STATUS_SUCCESS;
    if (flags & BCRYPT_BLOCK_PADDING)
    {
        if (output_len + key->u.s.block_size < *ret_len) return STATUS_BUFFER_TOO_SMALL;
        if (input_len < key->u.s.block_size) return STATUS_BUFFER_TOO_SMALL;
        bytes_left -= key->u.s.block_size;
    }
    else if (output_len < *ret_len) return STATUS_BUFFER_TOO_SMALL;

    if (key->u.s.mode == MODE_ID_ECB && iv) return STATUS_INVALID_PARAMETER;
    if ((status = key_symmetric_set_vector( key, iv, iv_len ))) return status;

    decrypt_params.key = key;
    decrypt_params.input = input;
    decrypt_params.input_len = key->u.s.block_size;
    decrypt_params.output = output;
    decrypt_params.output_len = key->u.s.block_size;
    while (bytes_left >= key->u.s.block_size)
    {
        if ((status = UNIX_CALL( key_symmetric_decrypt, &decrypt_params ))) return status;
        if (key->u.s.mode == MODE_ID_ECB && (status = key_symmetric_set_vector( key, NULL, 0 )))
            return status;
        bytes_left -= key->u.s.block_size;
        decrypt_params.input += key->u.s.block_size;
        decrypt_params.output += key->u.s.block_size;
    }

    if (flags & BCRYPT_BLOCK_PADDING)
    {
        UCHAR *buf, *dst = decrypt_params.output;
        if (!(buf = malloc( key->u.s.block_size ))) return STATUS_NO_MEMORY;
        decrypt_params.output = buf;
        status = UNIX_CALL( key_symmetric_decrypt, &decrypt_params );
        if (!status && buf[ key->u.s.block_size - 1 ] <= key->u.s.block_size)
        {
            *ret_len -= buf[ key->u.s.block_size - 1 ];
            if (output_len < *ret_len) status = STATUS_BUFFER_TOO_SMALL;
            else memcpy( dst, buf, key->u.s.block_size - buf[ key->u.s.block_size - 1 ] );
        }
        else status = STATUS_UNSUCCESSFUL; /* FIXME: invalid padding */
        free( buf );
    }

    return status;
}

static void key_destroy( struct key *key )
{
    if (key_is_symmetric( key ))
    {
        UNIX_CALL( key_symmetric_destroy, key );
        free( key->u.s.vector );
        free( key->u.s.secret );
        DeleteCriticalSection( &key->u.s.cs );
    }
    else
        UNIX_CALL( key_asymmetric_destroy, key );

    key->hdr.magic = 0;
    free( key );
}

static NTSTATUS key_import_pair( struct algorithm *alg, const WCHAR *type, BCRYPT_KEY_HANDLE *ret_key, UCHAR *input,
                                 ULONG input_len )
{
    struct key_asymmetric_import_params params;
    struct key *key;
    NTSTATUS status;
    ULONG size;

    if (!wcscmp( type, BCRYPT_ECCPUBLIC_BLOB ))
    {
        BCRYPT_ECCKEY_BLOB *ecc_blob = (BCRYPT_ECCKEY_BLOB *)input;
        DWORD key_size, magic;

        if (input_len < sizeof(*ecc_blob)) return STATUS_INVALID_PARAMETER;

        switch (alg->id)
        {
        case ALG_ID_ECDH_P256:
            key_size = 32;
            magic = BCRYPT_ECDH_PUBLIC_P256_MAGIC;
            break;

        case ALG_ID_ECDH_P384:
            key_size = 48;
            magic = BCRYPT_ECDH_PUBLIC_P384_MAGIC;
            break;

        case ALG_ID_ECDSA_P256:
            key_size = 32;
            magic = BCRYPT_ECDSA_PUBLIC_P256_MAGIC;
            break;

        case ALG_ID_ECDSA_P384:
            key_size = 48;
            magic = BCRYPT_ECDSA_PUBLIC_P384_MAGIC;
            break;

        default:
            FIXME( "algorithm %u does not yet support importing blob of type %s\n", alg->id, debugstr_w(type) );
            return STATUS_NOT_SUPPORTED;
        }

        if (ecc_blob->dwMagic != magic) return STATUS_INVALID_PARAMETER;
        if (ecc_blob->cbKey != key_size || input_len < sizeof(*ecc_blob) + ecc_blob->cbKey * 2)
            return STATUS_INVALID_PARAMETER;

        if ((status = key_asymmetric_create( alg->id, key_size * 8, &key ))) return status;
        params.key   = key;
        params.flags = KEY_IMPORT_FLAG_PUBLIC;
        params.buf   = input;
        params.len   = input_len;
        if ((status = UNIX_CALL( key_asymmetric_import, &params )))
        {
            key_destroy( key );
            return status;
        }

        *ret_key = key;
        return STATUS_SUCCESS;
    }
    else if (!wcscmp( type, BCRYPT_ECCPRIVATE_BLOB ))
    {
        BCRYPT_ECCKEY_BLOB *ecc_blob = (BCRYPT_ECCKEY_BLOB *)input;
        DWORD key_size, magic;

        if (input_len < sizeof(*ecc_blob)) return STATUS_INVALID_PARAMETER;

        switch (alg->id)
        {
        case ALG_ID_ECDH_P256:
            key_size = 32;
            magic = BCRYPT_ECDH_PRIVATE_P256_MAGIC;
            break;

        case ALG_ID_ECDH_P384:
            key_size = 48;
            magic = BCRYPT_ECDH_PRIVATE_P384_MAGIC;
            break;

        case ALG_ID_ECDSA_P256:
            key_size = 32;
            magic = BCRYPT_ECDSA_PRIVATE_P256_MAGIC;
            break;

        default:
            FIXME( "algorithm %u does not yet support importing blob of type %s\n", alg->id, debugstr_w(type) );
            return STATUS_NOT_SUPPORTED;
        }

        if (ecc_blob->dwMagic != magic) return STATUS_INVALID_PARAMETER;
        if (ecc_blob->cbKey != key_size || input_len < sizeof(*ecc_blob) + ecc_blob->cbKey * 3)
            return STATUS_INVALID_PARAMETER;

        if ((status = key_asymmetric_create( alg->id, key_size * 8, &key ))) return status;
        params.key   = key;
        params.flags = 0;
        params.buf   = input;
        params.len   = input_len;
        if ((status = UNIX_CALL( key_asymmetric_import, &params )))
        {
            key_destroy( key );
            return status;
        }

        *ret_key = key;
        return STATUS_SUCCESS;
    }
    else if (!wcscmp( type, BCRYPT_RSAPUBLIC_BLOB ))
    {
        BCRYPT_RSAKEY_BLOB *rsa_blob = (BCRYPT_RSAKEY_BLOB *)input;

        if (input_len < sizeof(*rsa_blob)) return STATUS_INVALID_PARAMETER;
        if ((alg->id != ALG_ID_RSA && alg->id != ALG_ID_RSA_SIGN) || rsa_blob->Magic != BCRYPT_RSAPUBLIC_MAGIC)
            return STATUS_NOT_SUPPORTED;

        size = sizeof(*rsa_blob) + rsa_blob->cbPublicExp + rsa_blob->cbModulus;
        if (size != input_len) return NTE_BAD_DATA;

        if ((status = key_asymmetric_create( alg->id, rsa_blob->BitLength, &key ))) return status;
        params.key   = key;
        params.flags = KEY_IMPORT_FLAG_PUBLIC;
        params.buf   = input;
        params.len   = input_len;
        if ((status = UNIX_CALL( key_asymmetric_import, &params )))
        {
            key_destroy( key );
            return status;
        }

        *ret_key = key;
        return STATUS_SUCCESS;
    }
    else if (!wcscmp( type, BCRYPT_RSAPRIVATE_BLOB ) || !wcscmp( type, BCRYPT_RSAFULLPRIVATE_BLOB ))
    {
        BCRYPT_RSAKEY_BLOB *rsa_blob = (BCRYPT_RSAKEY_BLOB *)input;

        if (input_len < sizeof(*rsa_blob)) return STATUS_INVALID_PARAMETER;
        if (alg->id != ALG_ID_RSA || (rsa_blob->Magic != BCRYPT_RSAPRIVATE_MAGIC &&
            rsa_blob->Magic != BCRYPT_RSAFULLPRIVATE_MAGIC)) return STATUS_NOT_SUPPORTED;

        if ((status = key_asymmetric_create( alg->id, rsa_blob->BitLength, &key ))) return status;
        params.key   = key;
        params.flags = 0;
        params.buf   = input;
        params.len   = input_len;
        if ((status = UNIX_CALL( key_asymmetric_import, &params )))
        {
            key_destroy( key );
            return status;
        }

        *ret_key = key;
        return STATUS_SUCCESS;
    }
    else if (!wcscmp( type, BCRYPT_DSA_PUBLIC_BLOB ))
    {
        BCRYPT_DSA_KEY_BLOB *dsa_blob = (BCRYPT_DSA_KEY_BLOB *)input;

        if (input_len < sizeof(*dsa_blob)) return STATUS_INVALID_PARAMETER;
        if ((alg->id != ALG_ID_DSA) || dsa_blob->dwMagic != BCRYPT_DSA_PUBLIC_MAGIC)
            return STATUS_NOT_SUPPORTED;

        if ((status = key_asymmetric_create( alg->id, dsa_blob->cbKey * 8, &key ))) return status;
        params.key   = key;
        params.flags = KEY_IMPORT_FLAG_PUBLIC;
        params.buf   = input;
        params.len   = input_len;
        if ((status = UNIX_CALL( key_asymmetric_import, &params )))
        {
            key_destroy( key );
            return status;
        }

        *ret_key = key;
        return STATUS_SUCCESS;
    }
    else if (!wcscmp( type, LEGACY_DSA_V2_PRIVATE_BLOB ))
    {
        BLOBHEADER *hdr = (BLOBHEADER *)input;
        DSSPUBKEY *pubkey;

        if (input_len < sizeof(*hdr)) return STATUS_INVALID_PARAMETER;

        if (hdr->bType != PRIVATEKEYBLOB && hdr->bVersion != 2 && hdr->aiKeyAlg != CALG_DSS_SIGN)
        {
            FIXME( "blob type %u version %u alg id %u not supported\n", hdr->bType, hdr->bVersion, hdr->aiKeyAlg );
            return STATUS_NOT_SUPPORTED;
        }
        if (alg->id != ALG_ID_DSA)
        {
            FIXME( "algorithm %u does not support importing blob of type %s\n", alg->id, debugstr_w(type) );
            return STATUS_NOT_SUPPORTED;
        }

        if (input_len < sizeof(*hdr) + sizeof(*pubkey)) return STATUS_INVALID_PARAMETER;
        pubkey = (DSSPUBKEY *)(hdr + 1);
        if (pubkey->magic != MAGIC_DSS2) return STATUS_NOT_SUPPORTED;

        if (input_len < sizeof(*hdr) + sizeof(*pubkey) + (pubkey->bitlen / 8) * 2 + 40 + sizeof(DSSSEED))
            return STATUS_INVALID_PARAMETER;

        if ((status = key_asymmetric_create( alg->id, pubkey->bitlen, &key ))) return status;
        key->u.a.flags |= KEY_FLAG_LEGACY_DSA_V2;
        params.key   = key;
        params.flags = 0;
        params.buf   = input;
        params.len   = input_len;
        if ((status = UNIX_CALL( key_asymmetric_import, &params )))
        {
            key_destroy( key );
            return status;
        }

        *ret_key = key;
        return STATUS_SUCCESS;
    }
    else if (!wcscmp( type, LEGACY_DSA_V2_PUBLIC_BLOB )) /* not supported on native */
    {
        BLOBHEADER *hdr = (BLOBHEADER *)input;
        DSSPUBKEY *pubkey;

        if (alg->id != ALG_ID_DSA) return STATUS_NOT_SUPPORTED;
        if (input_len < sizeof(*hdr)) return STATUS_INVALID_PARAMETER;

        if (hdr->bType != PUBLICKEYBLOB && hdr->bVersion != 2 && hdr->aiKeyAlg != CALG_DSS_SIGN)
        {
            FIXME( "blob type %u version %u alg id %u not supported\n", hdr->bType, hdr->bVersion, hdr->aiKeyAlg );
            return STATUS_NOT_SUPPORTED;
        }

        if (input_len < sizeof(*hdr) + sizeof(*pubkey)) return STATUS_INVALID_PARAMETER;
        pubkey = (DSSPUBKEY *)(hdr + 1);
        if (pubkey->magic != MAGIC_DSS1) return STATUS_NOT_SUPPORTED;

        size = sizeof(*hdr) + sizeof(*pubkey) + (pubkey->bitlen / 8) * 3 + 20 + sizeof(DSSSEED);
        if (input_len < size) return STATUS_INVALID_PARAMETER;

        if ((status = key_asymmetric_create( alg->id, pubkey->bitlen, &key ))) return status;
        key->u.a.flags |= KEY_FLAG_LEGACY_DSA_V2;
        params.key   = key;
        params.flags = KEY_IMPORT_FLAG_PUBLIC;
        params.buf   = input;
        params.len   = input_len;
        if ((status = UNIX_CALL( key_asymmetric_import, &params )))
        {
            key_destroy( key );
            return status;
        }

        *ret_key = key;
        return STATUS_SUCCESS;
    }

    FIXME( "unsupported key type %s\n", debugstr_w(type) );
    return STATUS_NOT_SUPPORTED;
}

static ULONG get_block_size( struct algorithm *alg )
{
    ULONG ret = 0, size = sizeof(ret);
    get_alg_property( alg, BCRYPT_BLOCK_LENGTH, (UCHAR *)&ret, sizeof(ret), &size );
    return ret;
}

NTSTATUS WINAPI BCryptGenerateSymmetricKey( BCRYPT_ALG_HANDLE algorithm, BCRYPT_KEY_HANDLE *handle,
                                            UCHAR *object, ULONG object_len, UCHAR *secret, ULONG secret_len,
                                            ULONG flags )
{
    BCRYPT_KEY_LENGTHS_STRUCT key_lengths;
    struct algorithm *alg = algorithm;
    ULONG block_size, size;
    struct key *key;

    TRACE( "%p, %p, %p, %lu, %p, %lu, %#lx\n", algorithm, handle, object, object_len, secret, secret_len, flags );

    if (!alg || alg->hdr.magic != MAGIC_ALG) return STATUS_INVALID_HANDLE;
    if (object) FIXME( "ignoring object buffer\n" );

    if (!bcrypt_handle)
    {
        ERR( "no encryption support\n" );
        return STATUS_NOT_IMPLEMENTED;
    }

    if (!(block_size = get_block_size( alg ))) return STATUS_INVALID_PARAMETER;

    if (!get_alg_property( alg, BCRYPT_KEY_LENGTHS, (UCHAR*)&key_lengths, sizeof(key_lengths), &size ))
    {
        if (secret_len > (size = key_lengths.dwMaxLength / 8))
        {
            WARN( "secret_len %lu exceeds key max length %lu, setting to maximum\n", secret_len, size );
            secret_len = size;
        }
        else if (secret_len < (size = key_lengths.dwMinLength / 8))
        {
            WARN( "secret_len %lu is less than minimum key length %lu\n", secret_len, size );
            return STATUS_INVALID_PARAMETER;
        }
        else if (key_lengths.dwIncrement && (secret_len * 8 - key_lengths.dwMinLength) % key_lengths.dwIncrement)
        {
            WARN( "secret_len %lu is not a valid key length\n", secret_len );
            return STATUS_INVALID_PARAMETER;
        }
    }

    if (!(key = calloc( 1, sizeof(*key) ))) return STATUS_NO_MEMORY;
    InitializeCriticalSection( &key->u.s.cs );
    key->hdr.magic      = MAGIC_KEY;
    key->alg_id         = alg->id;
    key->u.s.mode       = alg->mode;
    key->u.s.block_size = block_size;

    if (!(key->u.s.secret = malloc( secret_len )))
    {
        free( key );
        return STATUS_NO_MEMORY;
    }
    memcpy( key->u.s.secret, secret, secret_len );
    key->u.s.secret_len = secret_len;

    *handle = key;
    return STATUS_SUCCESS;
}

NTSTATUS WINAPI BCryptGenerateKeyPair( BCRYPT_ALG_HANDLE algorithm, BCRYPT_KEY_HANDLE *handle, ULONG key_len,
                                       ULONG flags )
{
    struct algorithm *alg = algorithm;
    struct key *key;
    NTSTATUS status;

    TRACE( "%p, %p, %lu, %#lx\n", algorithm, handle, key_len, flags );

    if (!alg || alg->hdr.magic != MAGIC_ALG) return STATUS_INVALID_HANDLE;
    if (!handle) return STATUS_INVALID_PARAMETER;

    if ((status = key_asymmetric_create( alg->id, key_len, &key ))) return status;
    *handle = key;
    return STATUS_SUCCESS;
}

NTSTATUS WINAPI BCryptFinalizeKeyPair( BCRYPT_KEY_HANDLE handle, ULONG flags )
{
    struct key *key = handle;

    TRACE( "%p, %#lx\n", key, flags );
    if (!key || key->hdr.magic != MAGIC_KEY) return STATUS_INVALID_HANDLE;

    return UNIX_CALL( key_asymmetric_generate, key );
}

NTSTATUS WINAPI BCryptImportKey( BCRYPT_ALG_HANDLE algorithm, BCRYPT_KEY_HANDLE decrypt_key, const WCHAR *type,
                                 BCRYPT_KEY_HANDLE *key, UCHAR *object, ULONG object_len, UCHAR *input,
                                 ULONG input_len, ULONG flags )
{
    struct algorithm *alg = algorithm;

    TRACE( "%p, %p, %s, %p, %p, %lu, %p, %lu, %#lx\n", algorithm, decrypt_key, debugstr_w(type), key, object,
          object_len, input, input_len, flags );

    if (!alg || alg->hdr.magic != MAGIC_ALG) return STATUS_INVALID_HANDLE;
    if (!key || !type || !input) return STATUS_INVALID_PARAMETER;

    if (decrypt_key)
    {
        FIXME( "decryption of key not yet supported\n" );
        return STATUS_NOT_IMPLEMENTED;
    }

    return key_import( algorithm, type, key, object, object_len, input, input_len );
}

NTSTATUS WINAPI BCryptExportKey( BCRYPT_KEY_HANDLE export_key, BCRYPT_KEY_HANDLE encrypt_key, const WCHAR *type,
                                 UCHAR *output, ULONG output_len, ULONG *size, ULONG flags )
{
    struct key *key = export_key;

    TRACE( "%p, %p, %s, %p, %lu, %p, %#lx\n", key, encrypt_key, debugstr_w(type), output, output_len, size, flags );

    if (!key || key->hdr.magic != MAGIC_KEY) return STATUS_INVALID_HANDLE;
    if (!type || !size) return STATUS_INVALID_PARAMETER;

    if (encrypt_key)
    {
        FIXME( "encryption of key not yet supported\n" );
        return STATUS_NOT_IMPLEMENTED;
    }

    return key_export( key, type, output, output_len, size );
}

static NTSTATUS key_duplicate( struct key *key_orig, struct key *key_copy )
{
    UCHAR *buffer;
    NTSTATUS status;

    memset( key_copy, 0, sizeof(*key_copy) );
    key_copy->hdr    = key_orig->hdr;
    key_copy->alg_id = key_orig->alg_id;

    if (key_is_symmetric( key_orig ))
    {
        if (!(buffer = malloc( key_orig->u.s.secret_len ))) return STATUS_NO_MEMORY;
        memcpy( buffer, key_orig->u.s.secret, key_orig->u.s.secret_len );

        key_copy->u.s.mode       = key_orig->u.s.mode;
        key_copy->u.s.block_size = key_orig->u.s.block_size;
        key_copy->u.s.secret     = buffer;
        key_copy->u.s.secret_len = key_orig->u.s.secret_len;
        InitializeCriticalSection( &key_copy->u.s.cs );
    }
    else
    {
        struct key_asymmetric_duplicate_params params;

        key_copy->u.a.bitlen   = key_orig->u.a.bitlen;
        key_copy->u.a.flags    = key_orig->u.a.flags;
        key_copy->u.a.dss_seed = key_orig->u.a.dss_seed;

        params.key_orig = key_orig;
        params.key_copy = key_copy;
        if ((status = UNIX_CALL( key_asymmetric_duplicate, &params ))) return status;
    }

    return STATUS_SUCCESS;
}

NTSTATUS WINAPI BCryptDuplicateKey( BCRYPT_KEY_HANDLE handle, BCRYPT_KEY_HANDLE *handle_copy,
                                    UCHAR *object, ULONG object_len, ULONG flags )
{
    struct key *key_orig = handle;
    struct key *key_copy;
    NTSTATUS status;

    TRACE( "%p, %p, %p, %lu, %#lx\n", handle, handle_copy, object, object_len, flags );
    if (object) FIXME( "ignoring object buffer\n" );

    if (!key_orig || key_orig->hdr.magic != MAGIC_KEY) return STATUS_INVALID_HANDLE;
    if (!handle_copy) return STATUS_INVALID_PARAMETER;
    if (!(key_copy = malloc( sizeof(*key_copy) ))) return STATUS_NO_MEMORY;

    if ((status = key_duplicate( key_orig, key_copy )))
    {
        key_destroy( key_copy );
        return status;
    }

    *handle_copy = key_copy;
    return STATUS_SUCCESS;
}

NTSTATUS WINAPI BCryptImportKeyPair( BCRYPT_ALG_HANDLE algorithm, BCRYPT_KEY_HANDLE decrypt_key, const WCHAR *type,
                                     BCRYPT_KEY_HANDLE *ret_key, UCHAR *input, ULONG input_len, ULONG flags )
{
    struct algorithm *alg = algorithm;

    TRACE( "%p, %p, %s, %p, %p, %lu, %#lx\n", algorithm, decrypt_key, debugstr_w(type), ret_key, input,
           input_len, flags );

    if (!alg || alg->hdr.magic != MAGIC_ALG) return STATUS_INVALID_HANDLE;
    if (!ret_key || !type || !input) return STATUS_INVALID_PARAMETER;
    if (decrypt_key)
    {
        FIXME( "decryption of key not yet supported\n" );
        return STATUS_NOT_IMPLEMENTED;
    }

    if (!wcscmp( type, BCRYPT_PUBLIC_KEY_BLOB ))
    {
        BCRYPT_KEY_BLOB *key_blob = (BCRYPT_KEY_BLOB *)input;

        if (input_len < sizeof(*key_blob)) return STATUS_INVALID_PARAMETER;

        switch (key_blob->Magic)
        {
        case BCRYPT_ECDH_PUBLIC_P256_MAGIC:
        case BCRYPT_ECDH_PUBLIC_P384_MAGIC:
        case BCRYPT_ECDSA_PUBLIC_P256_MAGIC:
        case BCRYPT_ECDSA_PUBLIC_P384_MAGIC:
            type = BCRYPT_ECCPUBLIC_BLOB;
            break;

        case BCRYPT_RSAPUBLIC_MAGIC:
            type = BCRYPT_RSAPUBLIC_BLOB;
            break;

        case BCRYPT_DSA_PUBLIC_MAGIC:
            type = BCRYPT_DSA_PUBLIC_BLOB;
            break;

        default:
            FIXME( "unsupported key magic %#lx\n", key_blob->Magic );
            return STATUS_NOT_SUPPORTED;
        }
    }

    return key_import_pair( alg, type, ret_key, input, input_len );
}

NTSTATUS WINAPI BCryptSignHash( BCRYPT_KEY_HANDLE handle, void *padding, UCHAR *input, ULONG input_len,
                                UCHAR *output, ULONG output_len, ULONG *ret_len, ULONG flags )
{
    struct key_asymmetric_sign_params params;
    struct key *key = handle;

    TRACE( "%p, %p, %p, %lu, %p, %lu, %p, %#lx\n", handle, padding, input, input_len, output, output_len,
           ret_len, flags );

    if (!key || key->hdr.magic != MAGIC_KEY) return STATUS_INVALID_HANDLE;
    if (key_is_symmetric( key ))
    {
        FIXME( "signing with symmetric keys not yet supported\n" );
        return STATUS_NOT_IMPLEMENTED;
    }

    params.key        = key;
    params.padding    = padding;
    params.input      = input;
    params.input_len  = input_len;
    params.output     = output;
    params.output_len = output_len;
    params.ret_len    = ret_len;
    params.flags      = flags;
    return UNIX_CALL( key_asymmetric_sign, &params );
}

NTSTATUS WINAPI BCryptVerifySignature( BCRYPT_KEY_HANDLE handle, void *padding, UCHAR *hash, ULONG hash_len,
                                       UCHAR *signature, ULONG signature_len, ULONG flags )
{
    struct key_asymmetric_verify_params params;
    struct key *key = handle;

    TRACE( "%p, %p, %p, %lu, %p, %lu, %#lx\n", handle, padding, hash, hash_len, signature, signature_len, flags );

    if (!key || key->hdr.magic != MAGIC_KEY) return STATUS_INVALID_HANDLE;
    if (!hash || !hash_len || !signature || !signature_len) return STATUS_INVALID_PARAMETER;
    if (key_is_symmetric( key )) return STATUS_NOT_SUPPORTED;

    params.key = key;
    params.padding = padding;
    params.hash = hash;
    params.hash_len = hash_len;
    params.signature = signature;
    params.signature_len = signature_len;
    params.flags = flags;
    return UNIX_CALL( key_asymmetric_verify, &params );
}

NTSTATUS WINAPI BCryptDestroyKey( BCRYPT_KEY_HANDLE handle )
{
    struct key *key = handle;

    TRACE( "%p\n", handle );

    if (!key || key->hdr.magic != MAGIC_KEY) return STATUS_INVALID_HANDLE;

    key_destroy( key );
    return STATUS_SUCCESS;
}

NTSTATUS WINAPI BCryptEncrypt( BCRYPT_KEY_HANDLE handle, UCHAR *input, ULONG input_len, void *padding, UCHAR *iv,
                               ULONG iv_len, UCHAR *output, ULONG output_len, ULONG *ret_len, ULONG flags )
{
    struct key *key = handle;
    struct key_asymmetric_encrypt_params asymmetric_params;
    NTSTATUS ret;

    TRACE( "%p, %p, %lu, %p, %p, %lu, %p, %lu, %p, %#lx\n", handle, input, input_len, padding, iv, iv_len, output,
           output_len, ret_len, flags );

    if (!key || key->hdr.magic != MAGIC_KEY) return STATUS_INVALID_HANDLE;

    if (key_is_symmetric( key ))
    {
        if (flags & ~BCRYPT_BLOCK_PADDING)
        {
            FIXME( "flags %#lx not implemented\n", flags );
            return STATUS_NOT_IMPLEMENTED;
        }
        EnterCriticalSection( &key->u.s.cs );
        ret = key_symmetric_encrypt( key, input, input_len, padding, iv, iv_len, output, output_len, ret_len, flags );
        LeaveCriticalSection( &key->u.s.cs );
    }
    else
    {
        if (flags & BCRYPT_PAD_NONE || flags & BCRYPT_PAD_OAEP)
        {
            FIXME( "flags %#lx not implemented\n", flags );
            return STATUS_NOT_IMPLEMENTED;
        }
        asymmetric_params.input = input;
        asymmetric_params.input_len = input_len;
        asymmetric_params.key = key;
        asymmetric_params.output = output;
        asymmetric_params.output_len = output_len;
        asymmetric_params.ret_len = ret_len;
        ret = UNIX_CALL(key_asymmetric_encrypt, &asymmetric_params);
    }

    return ret;
}

NTSTATUS WINAPI BCryptDecrypt( BCRYPT_KEY_HANDLE handle, UCHAR *input, ULONG input_len, void *padding, UCHAR *iv,
                               ULONG iv_len, UCHAR *output, ULONG output_len, ULONG *ret_len, ULONG flags )
{
    struct key_asymmetric_decrypt_params params;
    NTSTATUS ret;
    struct key *key = handle;

    TRACE( "%p, %p, %lu, %p, %p, %lu, %p, %lu, %p, %#lx\n", handle, input, input_len, padding, iv, iv_len, output,
           output_len, ret_len, flags );

    if (!key || key->hdr.magic != MAGIC_KEY) return STATUS_INVALID_HANDLE;

    if (key_is_symmetric( key ))
    {
        if (flags & ~BCRYPT_BLOCK_PADDING)
        {
            FIXME( "flags %#lx not supported\n", flags );
            return STATUS_NOT_IMPLEMENTED;
        }

        EnterCriticalSection( &key->u.s.cs );
        ret = key_symmetric_decrypt( key, input, input_len, padding, iv, iv_len, output, output_len, ret_len, flags );
        LeaveCriticalSection( &key->u.s.cs );
    }
    else
    {
        if (flags & BCRYPT_PAD_NONE || flags & BCRYPT_PAD_OAEP)
        {
            FIXME( "flags %#lx not implemented\n", flags );
            return STATUS_NOT_IMPLEMENTED;
        }
        params.key = key;
        params.input = input;
        params.input_len = input_len;
        params.output = output;
        params.output_len = output_len;
        params.ret_len = ret_len;
        ret = UNIX_CALL(key_asymmetric_decrypt, &params);
    }

    return ret;
}

NTSTATUS WINAPI BCryptSetProperty( BCRYPT_HANDLE handle, const WCHAR *prop, UCHAR *value, ULONG size, ULONG flags )
{
    struct object *object = handle;

    TRACE( "%p, %s, %p, %lu, %#lx\n", handle, debugstr_w(prop), value, size, flags );

    if (!object) return STATUS_INVALID_HANDLE;

    switch (object->magic)
    {
    case MAGIC_ALG:
    {
        struct algorithm *alg = (struct algorithm *)object;
        return set_alg_property( alg, prop, value, size, flags );
    }
    case MAGIC_KEY:
    {
        struct key *key = (struct key *)object;
        return set_key_property( key, prop, value, size, flags );
    }
    default:
        WARN( "unknown magic %#lx\n", object->magic );
        return STATUS_INVALID_HANDLE;
    }
}

#define HMAC_PAD_LEN 64
NTSTATUS WINAPI BCryptDeriveKeyCapi( BCRYPT_HASH_HANDLE handle, BCRYPT_ALG_HANDLE halg, UCHAR *key, ULONG keylen, ULONG flags )
{
    struct hash *hash = handle;
    UCHAR buf[MAX_HASH_OUTPUT_BYTES * 2];
    NTSTATUS status;
    ULONG len;

    TRACE( "%p, %p, %p, %lu, %#lx\n", handle, halg, key, keylen, flags );

    if (!key || !keylen) return STATUS_INVALID_PARAMETER;
    if (!hash || hash->hdr.magic != MAGIC_HASH) return STATUS_INVALID_HANDLE;
    if (keylen > builtin_algorithms[hash->alg_id].hash_length * 2) return STATUS_INVALID_PARAMETER;

    if (halg)
    {
        FIXME( "algorithm handle not supported\n" );
        return STATUS_NOT_IMPLEMENTED;
    }

    len = builtin_algorithms[hash->alg_id].hash_length;
    if ((status = hash_finalize( hash, buf, len ))) return status;

    if (len < keylen)
    {
        UCHAR pad1[HMAC_PAD_LEN], pad2[HMAC_PAD_LEN];
        ULONG i;

        for (i = 0; i < sizeof(pad1); i++)
        {
            pad1[i] = 0x36 ^ (i < len ? buf[i] : 0);
            pad2[i] = 0x5c ^ (i < len ? buf[i] : 0);
        }

        if ((status = hash_prepare( hash )) ||
            (status = hash_update( &hash->inner, hash->alg_id, pad1, sizeof(pad1) )) ||
            (status = hash_finalize( hash, buf, len ))) return status;

        if ((status = hash_prepare( hash )) ||
            (status = hash_update( &hash->inner, hash->alg_id, pad2, sizeof(pad2) )) ||
            (status = hash_finalize( hash, buf + len, len ))) return status;
    }

    memcpy( key, buf, keylen );
    return STATUS_SUCCESS;
}

static NTSTATUS pbkdf2( struct hash *hash, UCHAR *pwd, ULONG pwd_len, UCHAR *salt, ULONG salt_len,
                        ULONGLONG iterations, ULONG i, UCHAR *dst, ULONG hash_len )
{
    NTSTATUS status = STATUS_INVALID_PARAMETER;
    UCHAR bytes[4], *buf;
    ULONG j, k;

    if (!(buf = malloc( hash_len ))) return STATUS_NO_MEMORY;

    for (j = 0; j < iterations; j++)
    {
        if (j == 0)
        {
            /* use salt || INT(i) */
            if ((status = hash_update( &hash->inner, hash->alg_id, salt, salt_len )))
            {
                free( buf );
                return status;
            }
            bytes[0] = (i >> 24) & 0xff;
            bytes[1] = (i >> 16) & 0xff;
            bytes[2] = (i >> 8) & 0xff;
            bytes[3] = i & 0xff;
            status = hash_update( &hash->inner, hash->alg_id, bytes, 4 );
        }
        else status = hash_update( &hash->inner, hash->alg_id, buf, hash_len ); /* use U_j */

        if (status)
        {
            free( buf );
            return status;
        }

        if ((status = hash_finalize( hash, buf, hash_len )))
        {
            free( buf );
            return status;
        }

        if (j == 0) memcpy( dst, buf, hash_len );
        else for (k = 0; k < hash_len; k++) dst[k] ^= buf[k];
    }

    free( buf );
    return status;
}

NTSTATUS WINAPI BCryptDeriveKeyPBKDF2( BCRYPT_ALG_HANDLE handle, UCHAR *pwd, ULONG pwd_len, UCHAR *salt, ULONG salt_len,
                                       ULONGLONG iterations, UCHAR *dk, ULONG dk_len, ULONG flags )
{
    struct algorithm *alg = handle;
    ULONG hash_len, block_count, bytes_left, i;
    struct hash *hash;
    UCHAR *partial;
    NTSTATUS status;

    TRACE( "%p, %p, %lu, %p, %lu, %s, %p, %lu, %#lx\n", handle, pwd, pwd_len, salt, salt_len,
           wine_dbgstr_longlong(iterations), dk, dk_len, flags );

    if (!alg || alg->hdr.magic != MAGIC_ALG) return STATUS_INVALID_HANDLE;

    hash_len = builtin_algorithms[alg->id].hash_length;
    if (dk_len <= 0 || dk_len > ((((ULONGLONG)1) << 32) - 1) * hash_len) return STATUS_INVALID_PARAMETER;

    block_count = 1 + ((dk_len - 1) / hash_len); /* ceil(dk_len / hash_len) */
    bytes_left = dk_len - (block_count - 1) * hash_len;

    if ((status = hash_create( alg, pwd, pwd_len, BCRYPT_HASH_REUSABLE_FLAG, &hash ))) return status;

    /* full blocks */
    for (i = 1; i < block_count; i++)
    {
        if ((status = pbkdf2( hash, pwd, pwd_len, salt, salt_len, iterations, i, dk + ((i - 1) * hash_len), hash_len )))
        {
            hash_destroy( hash );
            return status;
        }
    }

    /* final partial block */
    if (!(partial = malloc( hash_len )))
    {
        hash_destroy( hash );
        return STATUS_NO_MEMORY;
    }

    if ((status = pbkdf2( hash, pwd, pwd_len, salt, salt_len, iterations, block_count, partial, hash_len )))
    {
        hash_destroy( hash );
        free( partial );
        return status;
    }
    memcpy( dk + ((block_count - 1) * hash_len), partial, bytes_left );

    hash_destroy( hash );
    free( partial );
    return STATUS_SUCCESS;
}

NTSTATUS WINAPI BCryptSecretAgreement( BCRYPT_KEY_HANDLE privatekey, BCRYPT_KEY_HANDLE publickey,
                                       BCRYPT_SECRET_HANDLE *handle, ULONG flags )
{
    struct key *privkey = privatekey;
    struct key *pubkey = publickey;
    struct secret *secret;

    FIXME( "%p, %p, %p, %#lx\n", privatekey, publickey, handle, flags );

    if (!privkey || privkey->hdr.magic != MAGIC_KEY) return STATUS_INVALID_HANDLE;
    if (!pubkey || pubkey->hdr.magic != MAGIC_KEY) return STATUS_INVALID_HANDLE;
    if (!handle) return STATUS_INVALID_PARAMETER;

    if (!(secret = calloc( 1, sizeof(*secret) ))) return STATUS_NO_MEMORY;
    secret->hdr.magic = MAGIC_SECRET;

    *handle = secret;
    return STATUS_SUCCESS;
}

NTSTATUS WINAPI BCryptDestroySecret(BCRYPT_SECRET_HANDLE handle)
{
    struct secret *secret = handle;

    FIXME( "%p\n", handle );

    if (!secret || secret->hdr.magic != MAGIC_SECRET) return STATUS_INVALID_HANDLE;
    secret->hdr.magic = 0;
    free( secret );
    return STATUS_SUCCESS;
}

NTSTATUS WINAPI BCryptDeriveKey( BCRYPT_SECRET_HANDLE handle, const WCHAR *kdf, BCryptBufferDesc *parameter,
                                 UCHAR *derived, ULONG derived_size, ULONG *result, ULONG flags )
{
    struct secret *secret = handle;

    FIXME( "%p, %s, %p, %p, %lu, %p, %#lx\n", secret, debugstr_w(kdf), parameter, derived, derived_size, result, flags );

    if (!secret || secret->hdr.magic != MAGIC_SECRET) return STATUS_INVALID_HANDLE;
    if (!kdf) return STATUS_INVALID_PARAMETER;

    return STATUS_INTERNAL_ERROR;
}

BOOL WINAPI DllMain( HINSTANCE hinst, DWORD reason, LPVOID reserved )
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls( hinst );
        if (!NtQueryVirtualMemory( GetCurrentProcess(), hinst, MemoryWineUnixFuncs,
                                   &bcrypt_handle, sizeof(bcrypt_handle), NULL ))
        {
            if (UNIX_CALL( process_attach, NULL)) bcrypt_handle = 0;
        }
        break;
    case DLL_PROCESS_DETACH:
        if (reserved) break;
        if (bcrypt_handle) UNIX_CALL( process_detach, NULL );
        break;
    }
    return TRUE;
}
