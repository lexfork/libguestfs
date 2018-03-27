# -*- python -*-
# coding: utf-8
# oVirt or RHV upload nbdkit plugin used by 'virt-v2v -o rhv-upload'
# Copyright (C) 2018 Red Hat Inc.
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License along
# with this program; if not, write to the Free Software Foundation, Inc.,
# 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

from __builtin__ import open as builtin_open
import json
import logging
import ssl
import sys
import time

from httplib import HTTPSConnection
from urlparse import urlparse

import ovirtsdk4 as sdk
import ovirtsdk4.types as types

# Timeout to wait for oVirt disks to change status, or the transfer
# object to finish initializing [seconds].
timeout = 5*60

# Parameters are passed in via a JSON doc from the OCaml code.
# Because this Python code ships embedded inside virt-v2v there
# is no formal API here.
# https://stackoverflow.com/a/13105359
def byteify(input):
    if isinstance(input, dict):
        return {byteify(key): byteify(value)
                for key, value in input.iteritems()}
    elif isinstance(input, list):
        return [byteify(element) for element in input]
    elif isinstance(input, unicode):
        return input.encode('utf-8')
    else:
        return input
params = None

def config(key, value):
    global params

    if key == "params":
        with builtin_open(value, 'r') as fp:
            params = byteify(json.load(fp))
    else:
        raise RuntimeError("unknown configuration key '%s'" % key)

def config_complete():
    if params is None:
        raise RuntimeError("missing configuration parameters")

def open(readonly):
    # Parse out the username from the output_conn URL.
    parsed = urlparse(params['output_conn'])
    username = parsed.username or "admin@internal"

    # Read the password from file.
    with builtin_open(params['output_password'], 'r') as fp:
        password = fp.read()
    password = password.rstrip()

    # Connect to the server.
    connection = sdk.Connection(
        url = params['output_conn'],
        username = username,
        password = password,
        ca_file = params['rhv_cafile'],
        log = logging.getLogger(),
        insecure = params['insecure'],
    )

    system_service = connection.system_service()

    # Create the disk.
    disks_service = system_service.disks_service()
    if params['disk_format'] == "raw":
        disk_format = types.DiskFormat.RAW
    else:
        disk_format = types.DiskFormat.COW
    disk = disks_service.add(
        disk = types.Disk(
            name = params['disk_name'],
            description = "Uploaded by virt-v2v",
            format = disk_format,
            initial_size = params['disk_size'],
            provisioned_size = params['disk_size'],
            # XXX Ignores params['output_sparse'].
            # Handling this properly will be complex, see:
            # https://www.redhat.com/archives/libguestfs/2018-March/msg00177.html
            sparse = False,
            storage_domains = [
                types.StorageDomain(
                    name = params['output_storage'],
                )
            ],
        )
    )

    # Wait till the disk is up, as the transfer can't start if the
    # disk is locked:
    disk_service = disks_service.disk_service(disk.id)

    endt = time.time() + timeout
    while True:
        time.sleep(5)
        disk = disk_service.get()
        if disk.status == types.DiskStatus.OK:
            break
        if time.time() > endt:
            raise RuntimeError("timed out waiting for disk to become unlocked")

    # Get a reference to the transfer service.
    transfers_service = system_service.image_transfers_service()

    # Create a new image transfer.
    transfer = transfers_service.add(
        types.ImageTransfer(
            image = types.Image(
                id = disk.id
            )
        )
    )

    # Get a reference to the created transfer service.
    transfer_service = transfers_service.image_transfer_service(transfer.id)

    # After adding a new transfer for the disk, the transfer's status
    # will be INITIALIZING.  Wait until the init phase is over. The
    # actual transfer can start when its status is "Transferring".
    endt = time.time() + timeout
    while True:
        time.sleep(5)
        transfer = transfer_service.get()
        if transfer.phase != types.ImageTransferPhase.INITIALIZING:
            break
        if time.time() > endt:
            raise RuntimeError("timed out waiting for transfer status " +
                               "!= INITIALIZING")

    # Now we have permission to start the transfer.
    if params['rhv_direct']:
        if transfer.transfer_url is None:
            raise RuntimeError("direct upload to host not supported, " +
                               "requires ovirt-engine >= 4.2 and only works " +
                               "when virt-v2v is run within the oVirt/RHV " +
                               "environment, eg. on an ovirt node.")
        destination_url = urlparse(transfer.transfer_url)
    else:
        destination_url = urlparse(transfer.proxy_url)

    context = ssl.create_default_context()
    context.load_verify_locations(cafile = params['rhv_cafile'])

    http = HTTPSConnection(
        destination_url.hostname,
        destination_url.port,
        context = context
    )

    # Save everything we need to make requests in the handle.
    return {
        'can_flush': False,
        'can_trim': False,
        'can_zero': False,
        'connection': connection,
        'disk': disk,
        'disk_service': disk_service,
        'failed': False,
        'got_options': False,
        'highestwrite': 0,
        'http': http,
        'needs_auth': not params['rhv_direct'],
        'path': destination_url.path,
        'transfer': transfer,
        'transfer_service': transfer_service,
    }

# Can we issue zero, trim or flush requests?
def get_options(h):
    if h['got_options']:
        return
    h['got_options'] = True

    http = h['http']
    transfer = h['transfer']

    http.putrequest("OPTIONS", h['path'])
    http.putheader("Authorization", transfer.signed_ticket)
    http.endheaders()

    r = http.getresponse()
    if r.status == 200:
        # New imageio never needs authentication.
        h['needs_auth'] = False

        j = byteify(json.loads(r.read()))
        h['can_zero'] = "zero" in j['features']
        h['can_trim'] = "trim" in j['features']
        h['can_flush'] = "flush" in j['features']

    # Old imageio servers returned either 405 Method Not Allowed or
    # 204 No Content (with an empty body).  If we see that we leave
    # all the features as False and they will be emulated.
    elif r.status == 405 or r.status == 204:
        pass

    else:
        raise RuntimeError("could not use OPTIONS request: %d: %s" %
                           (r.status, r.reason))

def can_trim(h):
    get_options(h)
    return h['can_trim']

def can_flush(h):
    get_options(h)
    return h['can_flush']

def get_size(h):
    return params['disk_size']

# For documentation see:
# https://github.com/oVirt/ovirt-imageio/blob/master/docs/random-io.md
# For examples of working code to read/write from the server, see:
# https://github.com/oVirt/ovirt-imageio/blob/master/daemon/test/server_test.py

def pread(h, count, offset):
    http = h['http']
    transfer = h['transfer']
    transfer_service = h['transfer_service']

    http.putrequest("GET", h['path'])
    # Authorization is only needed for old imageio.
    if h['needs_auth']:
        http.putheader("Authorization", transfer.signed_ticket)
    http.putheader("Range", "bytes=%d-%d" % (offset, offset+count-1))
    http.endheaders()

    r = http.getresponse()
    # 206 = HTTP Partial Content.
    if r.status != 206:
        h['transfer_service'].pause()
        h['failed'] = True
        raise RuntimeError("could not read sector (%d, %d): %d: %s" %
                           (offset, count, r.status, r.reason))
    return r.read()

def pwrite(h, buf, offset):
    http = h['http']
    transfer = h['transfer']
    transfer_service = h['transfer_service']

    count = len(buf)
    h['highestwrite'] = max(h['highestwrite'], offset+count)

    http.putrequest("PUT", h['path'] + "?flush=n")
    # Authorization is only needed for old imageio.
    if h['needs_auth']:
        http.putheader("Authorization", transfer.signed_ticket)
    # The oVirt server only uses the first part of the range, and the
    # content-length.
    http.putheader("Content-Range", "bytes %d-%d/*" % (offset, offset+count-1))
    http.putheader("Content-Length", str(count))
    http.endheaders()
    http.send(buf)

    r = http.getresponse()
    if r.status != 200:
        transfer_service.pause()
        h['failed'] = True
        raise RuntimeError("could not write sector (%d, %d): %d: %s" %
                           (offset, count, r.status, r.reason))

def zero(h, count, offset, may_trim):
    http = h['http']
    transfer = h['transfer']
    transfer_service = h['transfer_service']

    # Unlike the trim and flush calls, there is no 'can_zero' method
    # so nbdkit could call this even if the server doesn't support
    # zeroing.  If this is the case we must emulate.
    if not h['can_zero']:
        emulate_zero(h, count, offset)
        return

    # Construct the JSON request for zeroing.
    buf = json.dumps({'op': "zero",
                      'offset': offset,
                      'size': count,
                      'flush': False})

    http.putrequest("PATCH", h['path'])
    http.putheader("Content-Type", "application/json")
    http.putheader("Content-Length", len(buf))
    http.endheaders()
    http.send(buf)

    r = http.getresponse()
    if r.status != 200:
        transfer_service.pause()
        h['failed'] = True
        raise RuntimeError("could not zero sector (%d, %d): %d: %s" %
                           (offset, count, r.status, r.reason))

def emulate_zero(h, count, offset):
    # qemu-img convert starts by trying to zero/trim the whole device.
    # Since we've just created a new disk it's safe to ignore these
    # requests as long as they are smaller than the highest write seen.
    # After that we must emulate them with writes.
    if offset+count < h['highestwrite']:
        http.putrequest("PUT", h['path'])
        # Authorization is only needed for old imageio.
        if h['needs_auth']:
            http.putheader("Authorization", transfer.signed_ticket)
        http.putheader("Content-Range",
                       "bytes %d-%d/*" % (offset, offset+count-1))
        http.putheader("Content-Length", str(count))
        http.endheaders()

        buf = bytearray(128*1024)
        while count > len(buf):
            http.send(buf)
            count -= len(buf)
        http.send(buffer(buf, 0, count))

        r = http.getresponse()
        if r.status != 200:
            transfer_service.pause()
            h['failed'] = True
            raise RuntimeError("could not write zeroes (%d, %d): %d: %s" %
                               (offset, count, r.status, r.reason))

def trim(h, count, offset):
    http = h['http']
    transfer = h['transfer']
    transfer_service = h['transfer_service']

    # Construct the JSON request for trimming.
    buf = json.dumps({'op': "trim",
                      'offset': offset,
                      'size': count,
                      'flush': False})

    http.putrequest("PATCH", h['path'])
    http.putheader("Content-Type", "application/json")
    http.putheader("Content-Length", len(buf))
    http.endheaders()
    http.send(buf)

    r = http.getresponse()
    if r.status != 200:
        transfer_service.pause()
        h['failed'] = True
        raise RuntimeError("could not trim sector (%d, %d): %d: %s" %
                           (offset, count, r.status, r.reason))

def flush(h):
    http = h['http']
    transfer = h['transfer']
    transfer_service = h['transfer_service']

    # Construct the JSON request for flushing.
    buf = json.dumps({'op': "flush"})

    http.putrequest("PATCH", h['path'])
    http.putheader("Content-Type", "application/json")
    http.putheader("Content-Length", len(buf))
    http.endheaders()
    http.send(buf)

    r = http.getresponse()
    if r.status != 200:
        transfer_service.pause()
        h['failed'] = True
        raise RuntimeError("could not flush: %d: %s" % (r.status, r.reason))

def delete_disk_on_failure(h):
    disk_service = h['disk_service']
    disk_service.remove()

def close(h):
    http = h['http']
    connection = h['connection']

    # If the connection failed earlier ensure we clean up the disk.
    if h['failed']:
        delete_disk_on_failure(h)
        connection.close()
        return

    try:
        # Issue a flush request on close so that the data is written to
        # persistent store before we create the VM.
        if h['can_flush']:
            flush(h)

        http.close()

        disk = h['disk']
        transfer_service = h['transfer_service']

        transfer_service.finalize()

        # Wait until the transfer disk job is completed since
        # only then we can be sure the disk is unlocked.  As this
        # code is not very clear, what's happening is that we are
        # waiting for the transfer object to cease to exist, which
        # falls through to the exception case and then we can
        # continue.
        endt = time.time() + timeout
        try:
            while True:
                time.sleep(1)
                tmp = transfer_service.get()
                if time.time() > endt:
                    raise RuntimeError("timed out waiting for transfer " +
                                       "to finalize")
        except sdk.NotFoundError:
            pass

        # Write the disk ID file.  Only do this on successful completion.
        with builtin_open(params['diskid_file'], 'w') as fp:
            fp.write(disk.id)

    except:
        # Otherwise on any failure we must clean up the disk.
        delete_disk_on_failure(h)
        raise

    connection.close()
