# Reproducing the test VM

The first run used the 9front 11554 amd64 live ISO (January 2026) under QEMU,
with the kernel console on COM1. The stock ISO was modified only to add
`console=0` to `/cfg/plan9.ini`.

## 1. Rebuild the ISO

```sh
printf 'cdboot=yes\nconsole=0\nbootfile=/amd64/9pc64\n' > plan9.ini

xorriso -indev 9front-11554.amd64.iso \
  -outdev 9front-serial.iso \
  -compliance omit_version:no_force_dots \
  -rockridge off -joliet off \
  -map plan9.ini /cfg/plan9.ini \
  -boot_image any keep
```

Both compliance flags matter:

- `omit_version:no_force_dots` removes the `;1` suffix and trailing dot that
  xorriso adds to ISO9660 names. 9boot's ISO reader compares names literally
  and will otherwise stop at `386/9bootiso?`.
- `-rockridge off -joliet off` matches the stock layout. 9front's `9660srv`
  misparses xorriso's Rock Ridge system-use records and returns garbage
  directory entries.

## 2. Boot

```sh
qemu-system-x86_64 -enable-kvm -cpu host -m 2048 -smp 2 \
  -cdrom 9front-serial.iso -boot d -display none \
  -chardev socket,id=ser0,path=serial.sock,server=on,wait=off \
  -serial chardev:ser0 -monitor none \
  -netdev user,id=n0,hostfwd=tcp::15640-:5640 \
  -device e1000,netdev=n0
```

Connect to `serial.sock` with `socat` or a small expect script. At the
`bootargs` prompt press Enter, then Enter again at `user[glenda]:`.

`rio` fails because there is no frame buffer (`bind: #i: no frame buffer`),
and `init` falls back to `term%` on the serial line. That is the intended
state for this test.

## 3. Network and build

QEMU user networking gives the guest `10.0.2.15`; the host is `10.0.2.2`.

```rc
ip/ipconfig -g 10.0.2.2 ether /net/ether0 10.0.2.15 255.255.255.0
hget http://10.0.2.2:8000/vtfs.c > /tmp/vtfs.c
cd /tmp
6c vtfs.c
6l -o vtfs vtfs.6 /amd64/lib/lib9p.a
```

Serve the source from the host with:

```sh
python3 -m http.server 8000
```

## 4. Run

```rc
/tmp/vtfs -s vt -m /mnt/vt &
sleep 1
mount /srv/vt /mnt/vt
cat /mnt/vt/clone
echo 'echo hello' > /mnt/vt/1/input
sleep 1
cat /mnt/vt/1/screen
```

## Notes

- The live CD's `/bin` lacks `9660srv`; the bootfs has it in `/amd64/bin`.
  The stock boot path works once the ISO is rebuilt correctly.
- `tcp!*!5640` inside the guest is reachable from the guest itself via
  `srv -n tcp!10.0.2.15!5640`. QEMU user-mode `hostfwd` was unreliable for
  this server; a tap/bridged interface is recommended for host access.
