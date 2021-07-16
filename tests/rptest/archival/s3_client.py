import boto3
from botocore.config import Config
from botocore.exceptions import ClientError
from time import sleep
from functools import wraps
from collections import namedtuple
import traceback
import sys


class SlowDown(Exception):
    pass


S3ObjectMetadata = namedtuple('S3ObjectMetadata',
                              ['Bucket', 'Key', 'ETag', 'ContentLength'])


def retry_on_slowdown(tries=4, delay=1.0, backoff=2.0):
    """Retry with an exponential backoff if SlowDown exception was triggered."""
    def retry(fn):
        @wraps(fn)
        def do_retry(*args, **kwargs):
            ntries, sec_delay = tries, delay
            while ntries > 1:
                try:
                    return fn(*args, **kwargs)
                except SlowDown:
                    sleep(sec_delay)
                    ntries -= 1
                    sec_delay *= backoff
            # will stop filtering SlowDown exception when quota is reache
            return fn(*args, **kwargs)

        return do_retry

    return retry


class S3Client:
    """Simple S3 client"""
    def __init__(self,
                 region,
                 access_key,
                 secret_key,
                 endpoint,
                 logger,
                 disable_ssl=True):
        cfg = Config(region_name=region, signature_version='s3v4')
        self._cli = boto3.client('s3',
                                 config=cfg,
                                 aws_access_key_id=access_key,
                                 aws_secret_access_key=secret_key,
                                 endpoint_url=endpoint,
                                 use_ssl=not disable_ssl)
        self._region = region
        self.logger = logger

    def create_bucket(self, name):
        """Create bucket in S3"""
        try:
            self._cli.create_bucket(
                Bucket=name,
                CreateBucketConfiguration={'LocationConstraint': self._region})
        except ClientError as err:
            if err.response['Error']['Code'] != 'BucketAlreadyOwnedByYou':
                raise err

    def empty_bucket(self, name):
        """Empty bucket, return list of keys that wasn't deleted due
        to error"""
        keys = []
        try:
            self.logger.debug(f"running bucket cleanup on {name}")
            for obj in self.list_objects(bucket=name):
                keys.append(obj.Key)
                self.logger.debug(f"found key {obj.Key}")
        except Exception as e:
            # Expected to fail if bucket doesn't exist
            self.logger.debug(f"empty_bucket error: {e}")

        failed_keys = []
        for key in keys:
            self.logger.debug(f"deleting key {key}")
            try:
                reply = self._delete_object(name, key)
                self.logger.debug(f"delete request reply: {reply}")
            except:
                e, v = sys.exc_info()[:2]
                stacktrace = traceback.format_exc()
                self.logger.debug(
                    f"Delete request failed: {e} {v} {stacktrace}")
                failed_keys.append(key)
        return failed_keys

    def delete_object(self, bucket, key):
        """Remove object from S3"""
        return self._delete_object(bucket, key)

    @retry_on_slowdown()
    def _delete_object(self, bucket, key):
        """Remove object from S3"""
        try:
            return self._cli.delete_object(Bucket=bucket, Key=key)
        except ClientError as err:
            self.logger.debug(f"error response {err}")
            if err.response['Error']['Code'] == 'SlowDown':
                raise SlowDown()
            else:
                raise

    @retry_on_slowdown()
    def _get_object(self, bucket, key):
        """Get object from S3"""
        try:
            return self._cli.get_object(Bucket=bucket, Key=key)
        except ClientError as err:
            self.logger.debug(f"error response {err}")
            if err.response['Error']['Code'] == 'SlowDown':
                raise SlowDown()
            else:
                raise

    def get_object_data(self, bucket, key):
        resp = self._get_object(bucket, key)
        return resp['Body'].read()

    def get_object_meta(self, bucket, key):
        """Get object metadata without downloading it"""
        resp = self._get_object(bucket, key)
        # Note: ETag field contains md5 hash enclosed in double quotes that have to be removed
        return S3ObjectMetadata(Bucket=bucket,
                                Key=key,
                                ETag=resp['ETag'][1:-1],
                                ContentLength=resp['ContentLength'])

    def write_object_to_file(self, bucket, key, dest_path):
        """Get object and write it to file"""
        resp = self._get_object(bucket, key)
        with open(dest_path, 'wb') as f:
            body = resp['Body']
            for chunk in body.iter_chunks(chunk_size=0x1000):
                f.write(chunk)

    @retry_on_slowdown()
    def _list_objects(self, bucket, token=None, limit=1000):
        try:
            if token is not None:
                return self._cli.list_objects_v2(Bucket=bucket,
                                                 MaxKeys=limit,
                                                 ContinuationToken=token)
            else:
                return self._cli.list_objects_v2(Bucket=bucket, MaxKeys=limit)
        except ClientError as err:
            self.logger.debug(f"error response {err}")
            if err.response['Error']['Code'] == 'SlowDown':
                raise SlowDown()
            else:
                raise

    def list_objects(self, bucket):
        token = None
        truncated = True
        while truncated:
            res = self._list_objects(bucket, token, limit=100)
            token = res.get('NextContinuationToken')
            truncated = bool(res['IsTruncated'])
            if 'Contents' in res:
                for item in res['Contents']:
                    yield S3ObjectMetadata(Bucket=bucket,
                                           Key=item['Key'],
                                           ETag=item['ETag'][1:-1],
                                           ContentLength=item['Size'])
