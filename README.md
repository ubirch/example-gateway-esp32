# example-gateway-esp32

Prepare gateway memory.
```shell
$ python create_nvs_memory.py --stage demo --out gateway_memory.csv --token <insert your token here>
$ python $IDF_PATH/components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py generate gateway_memory.csv gateway_memory.bin 0x3000
$ parttool.py write_partition --partition-name=nvs --input gateway_memory.bin
```

Use `idf.py menuconfig` to set wifi ssid and password. Adapt path to ubirch backend (default is the prod stage).

**NOTE:** This is the new readme, which has to be setup. 
## Starting point is https://github.com/ubirch/example-esp32/tree/56b41d3b97045ea92f51a404250d68f4cbe8eca3
 


