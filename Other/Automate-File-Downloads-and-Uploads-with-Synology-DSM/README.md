# 说明

详细信息请参考： [debugwar.com/article/Automate-File-Downloads-and-Uploads-with-Synology-DSM](https://debugwar.com/article/Automate-File-Downloads-and-Uploads-with-Synology-DSM)

`synology-dsm-file.sh`用于自动化从DSM中下载或向DSM上传文件。

# 程序用法

```
Author:  Hacksign
Website: https://www.debugwar.com
GitHub:  https://github.com/Hacksign/Articles/tree/main/Other/Automate-File-Downloads-and-Uploads-with-Synology-DSM
Description:
  This script helps you download/upload file(s) from/to Synology DSM.
  NOTE: This script checks the second argument which is a filepath:
   * If the file doesn't exist, download file in sharing content.
   * If the file exists, upload file indicated by parameter 2.
Usage:
  download: synology-dsm-file.sh [sharing url] [filepath] [password]
    upload: synology-dsm-file.sh [sharing url] [filepath] [folder name] [password]
Example:
  download:
    synology-dsm-file.sh https://example.com/sharing/ab0cd1EFg /path/to/file.NO.exists
    synology-dsm-file.sh https://example.com/sharing/ab0cd1EFg /path/to/file.NO.exists pwd123
  upload:
    synology-dsm-file.sh https://example.com/sharing/ab0cd1EFg /path/to/file.exists FolderName
    synology-dsm-file.sh https://example.com/sharing/ab0cd1EFg /path/to/file.exists FolderName pwd123
```

