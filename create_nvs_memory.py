import argparse

parser = argparse.ArgumentParser()

parser.add_argument('--stage', type=str, default='prod', help='stage to use')
parser.add_argument('--token', type=str, help='token for id-service access',
                    required=True)
parser.add_argument('--out', type=str, help='name of output csv-file',
                    required=True)

args = parser.parse_args()

if args.stage == 'prod':
    backend_public_key = '74BIrQbAKFrwF3AJOBgwxGzsAl0B2GCF51pPAEHC5pA='
elif args.stage == 'dev' or args.stage == 'demo':
    backend_public_key = 'okA7krya3TZbPNEv8SDQIGR/hOppg/mLxMh+D0vozWY='
else:
    raise Exception('Unknown stage')

CSV_HEAD_TEMPLATE = '''key,type,encoding,value
key_storage,namespace,,
server_key,data,base64,{backend_public_key}
token,data,binary,{token}
'''

csv = CSV_HEAD_TEMPLATE.format(backend_public_key=backend_public_key,
                               token=args.token)

with open(args.out, 'w') as _f:
    _f.write(csv)
