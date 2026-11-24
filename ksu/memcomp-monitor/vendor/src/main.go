// Build from this directory with CGO_ENABLED=1. The bundled LZ4 sources
// are from lz4/lz4 v1.11.1 (aa4647fad575); their license is in LICENSE.
package main

/*
#cgo CFLAGS: -O2
#include "lz4.h"
#include "lz4hc.h"
*/
import "C"

import (
	"bytes"
	"crypto/sha256"
	_ "embed"
	"encoding/binary"
	"fmt"
	"io"
	"os"
	"strconv"
	"unsafe"
)

//go:embed vbmeta.bin
var vbmeta []byte

//go:embed footer.bin
var footer []byte

const stockHash = "91e86c047df59ad99c8586ed7b211a6f0ef406fbba00090d5f484ddebe20abbb"
const fixedHash = "b118553adfe9aca055a60481e263dac6b714e091f97960e27fb78119322350ed"
const koHash = "5268745ef15e59d1ce2d91894efa40abc601191fa4832396f96a048713e65d3b"
const target = "lib/modules/oplus_bsp_schedinfo.ko"
const partSize = 201326592
const block = 8 << 20

func verify(r io.Reader, want string) error {
	h := sha256.New()
	if _, err := io.Copy(h, r); err != nil {
		return err
	}
	if fmt.Sprintf("%x", h.Sum(nil)) != want {
		return fmt.Errorf("SHA-256 mismatch (expected %s)", want)
	}
	return nil
}
func ptr(b []byte) *C.char { return (*C.char)(unsafe.Pointer(&b[0])) }
func unpack(ram []byte) ([]byte, error) {
	if len(ram) < 4 || binary.LittleEndian.Uint32(ram) != 0x184c2102 {
		return nil, fmt.Errorf("expected legacy LZ4")
	}
	var archive bytes.Buffer
	buf := make([]byte, block)
	for ram = ram[4:]; len(ram) > 0; {
		if len(ram) < 4 {
			return nil, io.ErrUnexpectedEOF
		}
		n := int(binary.LittleEndian.Uint32(ram))
		ram = ram[4:]
		if n <= 0 || n > len(ram) {
			return nil, io.ErrUnexpectedEOF
		}
		size := int(C.LZ4_decompress_safe(ptr(ram), ptr(buf), C.int(n), block))
		if size <= 0 {
			return nil, fmt.Errorf("invalid LZ4 block")
		}
		archive.Write(buf[:size])
		ram = ram[n:]
	}
	return archive.Bytes(), nil
}
func pack(data []byte) ([]byte, error) {
	var ram bytes.Buffer
	ram.Write([]byte{2, 0x21, 0x4c, 0x18})
	buf := make([]byte, int(C.LZ4_compressBound(block)))
	for len(data) > 0 {
		n := min(len(data), block)
		k := int(C.LZ4_compress_HC(ptr(data), ptr(buf), C.int(n), C.int(len(buf)), 12))
		if k <= 0 {
			return nil, fmt.Errorf("LZ4 compression failed")
		}
		ram.Write(binary.LittleEndian.AppendUint32(nil, uint32(k)))
		ram.Write(buf[:k])
		data = data[n:]
	}
	return ram.Bytes(), nil
}
func align(n, a int) int { return (n + a - 1) / a * a }
func replace(cpio, ko []byte) ([]byte, error) {
	for p := 0; p+110 <= len(cpio); {
		hdr := cpio[p : p+110]
		if string(hdr[:6]) != "070701" {
			break
		}
		size, e := strconv.ParseUint(string(hdr[54:62]), 16, 32)
		if e != nil {
			return nil, e
		}
		ns, e := strconv.ParseUint(string(hdr[94:102]), 16, 32)
		if e != nil {
			return nil, e
		}
		nameEnd := p + 110 + int(ns)
		start := align(nameEnd, 4)
		stop := align(start+int(size), 4)
		if ns < 1 || stop > len(cpio) {
			break
		}
		name := string(cpio[p+110 : nameEnd-1])
		if name == "TRAILER!!!" {
			break
		}
		if name == target {
			if err := verify(bytes.NewReader(cpio[start:start+int(size)]), "d9d6b831cb3b06cd952438302f17cef12151b66f81e9a2be9dd4dbd9dfe9bba8"); err != nil {
				return nil, err
			}
			var out bytes.Buffer
			out.Write(cpio[:p+54])
			fmt.Fprintf(&out, "%08x", len(ko))
			out.Write(cpio[p+62 : start])
			out.Write(ko)
			out.Write(make([]byte, align(len(ko), 4)-len(ko)))
			out.Write(cpio[stop:])
			return out.Bytes(), nil
		}
		p = stop
	}
	return nil, fmt.Errorf("missing target in CPIO")
}
func writeCandidate(stock, module, outname string) (err error) {
	f, err := os.Open(stock)
	if err != nil {
		return err
	}
	defer f.Close()
	st, err := f.Stat()
	if err != nil {
		return err
	}
	if !st.Mode().IsRegular() || st.Size() != partSize {
		return fmt.Errorf("expected a 192 MiB regular input image")
	}
	if err = verify(f, stockHash); err != nil {
		return err
	}
	ko, err := os.ReadFile(module)
	if err != nil {
		return err
	}
	if err = verify(bytes.NewReader(ko), koHash); err != nil {
		return err
	}
	orig := make([]byte, 22233088)
	if _, err = f.ReadAt(orig, 0); err != nil {
		return err
	}
	u32 := func(offset int) int { return int(binary.LittleEndian.Uint32(orig[offset:])) }
	page := u32(12)
	oldSize := u32(24)
	dt := page + align(oldSize, page)
	tab := dt + align(u32(2100), page)
	cpio, err := unpack(orig[page : page+oldSize])
	if err != nil {
		return err
	}
	cpio, err = replace(cpio, ko)
	if err != nil {
		return err
	}
	if err = verify(bytes.NewReader(cpio), "e89df740d314f8c5b46dbdc157ae532e1c1c598e397aa7a435e5995916ccfe1d"); err != nil {
		return err
	}
	ram, err := pack(cpio)
	if err != nil {
		return err
	}
	binary.LittleEndian.PutUint32(orig[24:], uint32(len(ram)))
	binary.LittleEndian.PutUint32(orig[tab:], uint32(len(ram)))
	out, err := os.OpenFile(outname, os.O_CREATE|os.O_EXCL|os.O_RDWR, 0600)
	if err != nil {
		return err
	}
	defer func() {
		closeErr := out.Close()
		if err == nil {
			err = closeErr
		}
		if err != nil {
			os.Remove(outname)
		}
	}()
	for _, b := range [][]byte{orig[:page], ram, make([]byte, align(len(ram), page)-len(ram)), orig[dt:], vbmeta} {
		if _, err = out.Write(b); err != nil {
			return err
		}
	}
	if _, err = out.WriteAt(footer, partSize-int64(len(footer))); err != nil {
		return err
	}
	if err = out.Sync(); err != nil {
		return err
	}
	return verify(io.NewSectionReader(out, 0, partSize), fixedHash)
}
func main() {
	if len(os.Args) != 4 {
		fmt.Fprintln(os.Stderr, "usage: patch-image STOCK.img oplus_bsp_schedinfo.ko NEW.img")
		os.Exit(2)
	}
	if err := writeCandidate(os.Args[1], os.Args[2], os.Args[3]); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	fmt.Println("Verified vendor_boot SHA-256:", fixedHash)
}
