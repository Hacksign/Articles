#!/bin/sh
#
# Copyright (C) 2026 Hacksign <https://www.debugwar.com>
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 2
# of the License, or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
#

SHARING_URL="$1"
SHARING_FILEPATH="$2"
PARAMETER3="$3"
PARAMETER4="$4"

usage()
{
    script_name=$(basename "$0")
    error_message="$1"

    echo "Author:  Hacksign"
    echo "Website: https://www.debugwar.com"
    echo "GitHub:  https://github.com/Hacksign/Articles/tree/main/Other/Automate-File-Downloads-and-Uploads-with-Synology-DSM"
    echo "License: GPLv2"
    echo "Description:"
    echo "  This script helps you download/upload file(s) from/to Synology DSM."
    echo "  NOTE: This script checks the second argument which is a filepath:"
    echo "   * If the file doesn't exist, download file in sharing content."
    echo "   * If the file exists, upload file indicated by parameter 2."
    echo "Usage:"
    echo "  download: ${script_name} [sharing url] [filepath] [password]"
    echo "    upload: ${script_name} [sharing url] [filepath] [folder name] [password]"
    echo "Example:"
    echo "  download:"
    echo "    ${script_name} https://example.com/sharing/ab0cd1EFg /path/to/file.NO.exists"
    echo "    ${script_name} https://example.com/sharing/ab0cd1EFg /path/to/file.NO.exists pwd123"
    echo "  upload:"
    echo "    ${script_name} https://example.com/sharing/ab0cd1EFg /path/to/file.exists FolderName"
    echo "    ${script_name} https://example.com/sharing/ab0cd1EFg /path/to/file.exists FolderName pwd123"

    if [[ "${error_message}x" != "x" ]]; then
        echo ""
        echo "[!] ${error_message}"
    fi
}

get_sharing_sid()
{
    sharing_host="$1"
    sharing_id="$2"
    sharing_pwd="$3"

    # request sharing_sid parameter we need next step
    response=$(curl -kvL \
        "${sharing_host}/sharing/webapi/entry.cgi/SYNO.Core.Sharing.Login" \
        --data-raw "api=SYNO.Core.Sharing.Login&method=login&version=1&sharing_id=%22${sharing_id}%22&password=%22${sharing_pwd}%22" 2>/dev/null)
    sharing_sid=$(echo ${response} | sed -n 's/.*"sharing_sid":"\(.*\)"\s*}.*/\1/p')
    if [ -z "${sharing_sid}" ]; then
        return 255
    fi

    echo ${sharing_sid}
    return 0
}

upload_to_dsm()
{
    sharing_url="$1"
    upload_filepath="$2"
    sharing_folder="$3"
    sharing_pwd="$4"
    filename=$(basename ${upload_filepath} 2>/dev/null | tr -d '\n')
    filesize=$(wc -c < "${upload_filepath}" 2>/dev/null | tr -d '\n')

    # split sharing_url by /, we need schema, host, port, share id
    old_ifs="$IFS"
    IFS='/'
    set -- $sharing_url
    sharing_host="$1//$3"
    sharing_id="$5"
    IFS="$old_ifs"

    # request sharing_sid parameter we need next step
    sharing_sid=$(get_sharing_sid "${sharing_host}" "${sharing_id}" "${sharing_pwd}")
    if [ $? -eq 0 ]; then
        #check write permission
        response=$(curl -kvL \
            "${sharing_host}/sharing/webapi/entry.cgi" \
            -H "X-SYNO-SHARING: ${sharing_id}" \
            -H "Cookie: sharing_sid=${sharing_sid}" \
            --data-raw "api=SYNO.FileStation.CheckPermission&method=write&version=3&filename=%22${filename}%22&size=${filesize}&overwrite=true&sharing_id=%22${sharing_id}%22&uploader_name=%22${sharing_folder}%22" 2>/dev/null)
        has_write_permission=$(echo ${response} | sed -n 's/"success":true/\1/p')
        if [ ! -z "${has_write_permission}" ]; then
            # upload file with curl
            response=$(curl -kvL \
                "${sharing_host}/webapi/entry.cgi?api=SYNO.FileStation.Upload&method=upload&version=2&_sharing_id=${sharing_id}" \
                -H "Cookie: sharing_sid=${sharing_sid}" \
                -F "overwrite=true" \
                -F "mtime=$(date +%s)000" \
                -F "sharing_id=${sharing_id}" \
                -F "uploader_name=${sharing_folder}" \
                -F "files=@${upload_filepath}" 2>/dev/null)
            upload_successed=$(echo ${response} | sed -n 's/"success":true/\1/p')
            if [ ! -z "${upload_successed}" ]; then
                return 0
            fi
        fi
    fi
    return 255
}

download_from_dsm()
{
    sharing_url="$1"
    download_path="$2"
    sharing_pwd="$3"

    # split sharing_url by /, we need schema, host, port, share id
    old_ifs="$IFS"
    IFS='/'
    set -- $sharing_url
    sharing_host="$1//$3"
    sharing_id="$5"
    IFS="$old_ifs"

    # check parameters we need in next step, if empty return false
    if [[ "${sharing_host}x" == "x" ]]; then
        return 255
    elif [[ "${sharing_id}x" == "x" ]]; then
        return 255
    fi

    # request sharing_sid parameter we need next step
    sharing_sid=$(get_sharing_sid "${sharing_host}" "${sharing_id}" "${sharing_pwd}")
    if [ $? -eq 0 ]; then
        # request sharing_filname we need when downloading
        # there are two situations, with and without passwd
        # the request url is different, check the input password parameter to indicate which situation is
        if [[ "${sharing_pwd}x" == "x" ]]; then
            response=$(curl -kvL \
                "${sharing_host}/sharing/webapi/entry.cgi?api=SYNO.Core.Sharing.Session&version=1&method=get&sharing_id=%22${sharing_id}%22" \
                -H "Cookie: sharing_sid=${sharing_sid}" 2>/dev/null)
            sharing_filename=$(echo ${response} | sed -E 's#.*"filename"\s*:\s*"([^"]*)"\s*,.*#\1#')
        else
            response=$(curl -kvL \
                "${sharing_host}/sharing/webapi/entry.cgi" \
                -H "X-SYNO-SHARING: ${sharing_id}" \
                -H "Cookie: sharing_sid=${sharing_sid}" \
                --data-raw 'api=SYNO.Core.Sharing.Initdata&method=get&version=1' 2>/dev/null)
            sharing_filename=$(echo ${response} | sed -E 's/.*"Private"\s*:\s*\{\s*"filename"\s*:\s*"([^"]*)".*/\1/')
        fi
        if [[ "${sharing_filename}x" == "x" ]]; then
            return 255
        fi

        # do the download work
        response=$(curl -kvL \
            "${sharing_host}/fsdownload/${sharing_id}/${sharing_filename}" \
            -H "Cookie: sharing_sid=${sharing_sid}" \
            -o "${download_path}" 2>/dev/null)
        if [[ $? == 0 ]]; then
            return 0
        fi
    fi
    return 255
}

# check input parameter
if [[ $# == 0 ]]; then
    usage
    exit
elif [[ "${SHARING_URL}x" == "x" ]]; then
    usage "missing sharing url"
    exit
elif [[ "${SHARING_FILEPATH}x" == "x" ]]; then
    usage "missing sharing path"
    exit
fi

if [[ -e "${SHARING_FILEPATH}" ]]; then
    # when it is upload mode, parameter 3 is folder, parameter 4 is password
    upload_to_dsm "${SHARING_URL}" "${SHARING_FILEPATH}" "${PARAMETER3}" "${PARAMETER4}"
    if [[ $? == 0 ]]; then
        echo "[*] upload success."
    else
        echo "[!] upload failed."
    fi
else
    # when it is upload mode, parameter 3 is password, there is no parameter 4
    download_from_dsm "${SHARING_URL}" "${SHARING_FILEPATH}" "${PARAMETER3}"
    if [[ $? == 0 ]]; then
        echo "[*] download success."
    else
        echo "[!] download failed."
    fi
fi

