#!/usr/bin/env python3
"""s3_sync.py - zero-dependency S3 client for the UnrealGamePatcher CDN.

Publishes a local CDN root (Saved\\UGP_Package\\CloudDir) into any
S3-compatible object store (rustfs / MinIO / AWS S3 / Aliyun OSS / Tencent
COS ...). Uses only the Python standard library (SigV4 signing) so it works
anywhere without installing aws-cli / mc / boto3.

Usage:
  python s3_sync.py --endpoint http://127.0.0.1:9000 \
      --access-key ugpadmin --secret-key ugpadmin123 \
      --bucket ugp-cdn --public \
      --sync D:/UEProject/IOStoreDemo/Saved/UGP_Package/CloudDir

  --create    create the bucket if missing
  --public    set a public-read bucket policy (clients GET without signing)
  --sync DIR  recursively upload DIR/* into the bucket root (S3 PUT, SigV4)

Client access afterwards: -cdn=http://host:9000/<bucket> (a public bucket
serves the patcher's plain HTTP GET + Range unsigned; -cdn=file:///... for a
local copy).
"""

import argparse
import datetime
import hashlib
import hmac
import os
import sys
import urllib.parse
import urllib.error
import urllib.request

# ---------------------------------------------------------------------------
# AWS Signature Version 4 (S3 path-style, standard library only)
# ---------------------------------------------------------------------------

def _hmac(key, msg):
    return hmac.new(key, msg.encode("utf-8"), hashlib.sha256).digest()

def _sig_key(secret, date, region):
    k_date = _hmac(("AWS4" + secret).encode("utf-8"), date)
    k_region = _hmac(k_date, region)
    k_service = _hmac(k_region, "s3")
    return _hmac(k_service, "aws4_request")

def sign_headers(method, url, access_key, secret_key, region, payload=b""):
    """Returns dict of Authorization / x-amz-date / x-amz-content-sha256."""
    parsed = urllib.parse.urlsplit(url)
    now = datetime.datetime.now(datetime.timezone.utc)
    amz_date = now.strftime("%Y%m%dT%H%M%SZ")
    date = now.strftime("%Y%m%d")

    # Split path into bucket/key for path-style signing.
    path = parsed.path or "/"
    canonical_uri = urllib.parse.quote(path, safe="/")

    # S3 subresources (e.g. ?policy) are part of the signature; normalize the
    # query by sorting the parameters.
    query_params = urllib.parse.parse_qsl(parsed.query, keep_blank_values=True)
    canonical_query = urllib.parse.urlencode(sorted(query_params))

    payload_hash = hashlib.sha256(payload).hexdigest()
    headers = {
        "host": parsed.netloc,
        "x-amz-content-sha256": payload_hash,
        "x-amz-date": amz_date,
    }
    signed_headers = ";".join(sorted(headers))
    canonical_headers = "".join(
        f"{k}:{v}\n" for k, v in sorted(headers.items()))
    canonical_request = "\n".join([
        method, canonical_uri, canonical_query, canonical_headers,
        signed_headers, payload_hash])
    scope = f"{date}/{region}/s3/aws4_request"
    string_to_sign = "\n".join([
        "AWS4-HMAC-SHA256", amz_date, scope,
        hashlib.sha256(canonical_request.encode("utf-8")).hexdigest()])
    signature = hmac.new(_sig_key(secret_key, date, region),
                         string_to_sign.encode("utf-8"),
                         hashlib.sha256).hexdigest()
    headers["authorization"] = (
        f"AWS4-HMAC-SHA256 Credential={access_key}/{scope}, "
        f"SignedHeaders={signed_headers}, Signature={signature}")
    return headers

def s3_request(method, url, access_key, secret_key, region, payload=b""):
    headers = sign_headers(method, url, access_key, secret_key, region, payload)
    req = urllib.request.Request(url, data=payload if method in ("PUT", "POST") else None,
                                 method=method, headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=120) as resp:
            return resp.status, resp.read()
    except urllib.error.HTTPError as e:
        return e.code, e.read()
    except urllib.error.URLError as e:
        print(f"[s3] ERROR connection failed: {e.reason}")
        return 0, b""

# ---------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--endpoint", required=True)
    ap.add_argument("--access-key", required=True)
    ap.add_argument("--secret-key", required=True)
    ap.add_argument("--bucket", required=True)
    ap.add_argument("--region", default="us-east-1")
    ap.add_argument("--create", action="store_true")
    ap.add_argument("--public", action="store_true")
    ap.add_argument("--sync", help="local dir to upload into the bucket root")
    args = ap.parse_args()

    ep = args.endpoint.rstrip("/")

    def url(key=""):
        # Path-style: /<bucket>/<key>
        quoted = urllib.parse.quote(key)
        return f"{ep}/{args.bucket}/{quoted}" if key else f"{ep}/{args.bucket}"

    if args.create:
        code, body = s3_request("PUT", url(), args.access_key, args.secret_key,
                                args.region)
        if code in (200, 409):  # 409 = already exists
            print(f"[s3] bucket '{args.bucket}' ready (PUT {code})")
        else:
            print(f"[s3] ERROR creating bucket: {code} {body[:200]}")
            return 1

    if args.public:
        policy = (
            '{"Version":"2012-10-17","Statement":[{"Effect":"Allow",'
            '"Principal":"*","Action":["s3:GetObject"],'
            f'"Resource":"arn:aws:s3:::{args.bucket}/*"}}]}}'
        )
        code, body = s3_request(
            "PUT", url() + "?policy", args.access_key, args.secret_key,
            args.region, payload=policy.encode("utf-8"))
        # 200 or 204 (No Content) both mean the policy was accepted.
        print(f"[s3] public-read policy set ({code})" if code in (200, 204)
              else f"[s3] WARN policy PUT {code} {body[:200]}")

    if args.sync:
        root = os.path.abspath(args.sync)
        if not os.path.isdir(root):
            print(f"[s3] ERROR --sync dir not found: {root} (a typo here would publish an EMPTY bucket)")
            return 1
        uploaded = 0
        total_bytes = 0
        for dirpath, _dirs, files in os.walk(root):
            for name in files:
                local = os.path.join(dirpath, name)
                key = os.path.relpath(local, root).replace("\\", "/")
                with open(local, "rb") as f:
                    data = f.read()
                code, body = s3_request("PUT", url(key), args.access_key,
                                        args.secret_key, args.region,
                                        payload=data)
                if code != 200:
                    print(f"[s3] ERROR {key}: {code} {body[:200]}")
                    return 1
                uploaded += 1
                total_bytes += len(data)
                if uploaded % 500 == 0:
                    print(f"[s3] {uploaded} objects, {total_bytes//(1<<20)} MiB...")
        print(f"[s3] uploaded {uploaded} objects, {total_bytes/(1<<20):.1f} MiB -> {args.bucket}")

    return 0

if __name__ == "__main__":
    sys.exit(main())
