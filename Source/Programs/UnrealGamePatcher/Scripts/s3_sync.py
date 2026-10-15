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

Transient failures (connection reset / timeout / 5xx) are retried with
exponential backoff; re-runs resume by default -- already-published objects
(HEAD + MD5 or size match) are skipped, --full forces a complete re-upload.

Client access afterwards: -cdn=http://host:9000/<bucket> (a public bucket
serves the patcher's plain HTTP GET + Range unsigned; -cdn=file:///... for a
local copy).
"""

import argparse
import datetime
import hashlib
import hmac
import http.client
import os
import socket
import sys
import time
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

_TRANSIENT_EXC = (urllib.error.URLError, socket.timeout, ConnectionError,
                  http.client.HTTPException)

def s3_request(method, url, access_key, secret_key, region, payload=b"",
               attempts=5):
    """S3 request with exponential-backoff retry on transient failures.

    Retries connection errors and 429/5xx; 4xx responses are definitive.
    Returns (status, body); status 0 means the request never got a response.
    """
    for attempt in range(attempts):
        headers = sign_headers(method, url, access_key, secret_key, region, payload)
        req = urllib.request.Request(
            url, data=payload if method in ("PUT", "POST") else None,
            method=method, headers=headers)
        try:
            with urllib.request.urlopen(req, timeout=120) as resp:
                return resp.status, resp.read()
        except urllib.error.HTTPError as e:
            body = e.read()
            if e.code in (429, 500, 502, 503, 504) and attempt < attempts - 1:
                err = f"HTTP {e.code}"
            else:
                return e.code, body
        except _TRANSIENT_EXC as e:
            err = getattr(e, "reason", e)
            if attempt >= attempts - 1:
                print(f"[s3] ERROR connection failed after {attempts} attempts: {err}")
                return 0, b""
        wait = 2 ** attempt
        print(f"[s3] attempt {attempt + 1} failed ({err}), retrying in {wait}s...")
        time.sleep(wait)
    return 0, b""

def s3_head(url, access_key, secret_key, region):
    """HEAD an object; returns (size, etag) or None when it does not exist.

    Returns (-1, None) when the check itself failed (caller should upload).
    """
    headers = sign_headers("HEAD", url, access_key, secret_key, region)
    req = urllib.request.Request(url, method="HEAD", headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=60) as resp:
            size = int(resp.headers.get("Content-Length", -1))
            return size, resp.headers.get("ETag")
    except urllib.error.HTTPError as e:
        e.read()
        return None if e.code == 404 else (-1, None)
    except _TRANSIENT_EXC:
        return (-1, None)

def file_md5(path):
    h = hashlib.md5()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()

def remote_matches_local(head, local):
    """True when the remote object is already the same content as local.

    Compares MD5 when the ETag is a plain single-part MD5 ("hex" form);
    multipart ETags ("hex-N") and unknown states fall back to size only.
    """
    size, etag = head
    if size != os.path.getsize(local):
        return False
    if etag and not etag.strip('"').endswith("-"):
        try:
            return etag.strip('"') == file_md5(local)
        except OSError:
            return False
    return True

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
    ap.add_argument("--full", action="store_true",
                    help="re-upload everything (default: skip objects already "
                         "in the bucket with matching content)")
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
        total = sum(len(fs) for _dp, _dn, fs in os.walk(root))
        uploaded = skipped = failed = 0
        total_bytes = 0
        failures = []
        consecutive_failures = 0
        for dirpath, _dirs, files in os.walk(root):
            for name in files:
                local = os.path.join(dirpath, name)
                key = os.path.relpath(local, root).replace("\\", "/")
                if not args.full:
                    head = s3_head(url(key), args.access_key,
                                   args.secret_key, args.region)
                    if head and head != (-1, None) and remote_matches_local(head, local):
                        skipped += 1
                        continue
                with open(local, "rb") as f:
                    data = f.read()
                code, body = s3_request("PUT", url(key), args.access_key,
                                        args.secret_key, args.region,
                                        payload=data)
                if code != 200:
                    print(f"[s3] ERROR {key}: {code} {body[:200]}")
                    failures.append(key)
                    failed += 1
                    consecutive_failures += 1
                    # Give up only when the endpoint itself looks dead, so one
                    # bad object never aborts a 10k-object publish.
                    if consecutive_failures >= 10:
                        print("[s3] ERROR 10 consecutive failures, aborting (endpoint down?)")
                        return 1
                    continue
                uploaded += 1
                total_bytes += len(data)
                consecutive_failures = 0
                done = uploaded + skipped + failed
                if done % 500 == 0:
                    print(f"[s3] {done}/{total} objects, {uploaded} uploaded "
                          f"({total_bytes//(1<<20)} MiB), {skipped} skipped, {failed} failed...")
        print(f"[s3] uploaded {uploaded} objects ({total_bytes/(1<<20):.1f} MiB), "
              f"skipped {skipped}, failed {failed} -> {args.bucket}")
        if failures:
            for k in failures[:20]:
                print(f"[s3]   failed: {k}")
            if len(failures) > 20:
                print(f"[s3]   ... and {len(failures) - 20} more")
            return 1

    return 0

if __name__ == "__main__":
    sys.exit(main())
