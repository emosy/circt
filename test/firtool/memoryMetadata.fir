; RUN: firtool %s -dedup=false --lower-memory --format=fir --annotation-file %s.anno.json --repl-seq-mem --repl-seq-mem-file="metadata/dutModule.conf" --verilog |  FileCheck %s


circuit test:
  module tbMemModule1:
    input clock: Clock
    input rAddr: UInt<4>
    input rEn: UInt<1>
    output rData: UInt<8>
    input wMask: UInt<1>
    input wData: UInt<8>

    mem tbMemoryKind1:
      data-type => UInt<8>
      depth => 16
      reader => r
      writer => w
      read-latency => 1
      write-latency => 1
      read-under-write => undefined

    tbMemoryKind1.r.clk <= clock
    tbMemoryKind1.r.en <= rEn
    tbMemoryKind1.r.addr <= rAddr
    rData <= tbMemoryKind1.r.data
    tbMemoryKind1.w.clk <= clock
    tbMemoryKind1.w.en <= rEn
    tbMemoryKind1.w.addr <= rAddr
    tbMemoryKind1.w.mask <= wMask
    tbMemoryKind1.w.data <= wData

; CHECK-LABEL: module tbMemModule1
; CHECK:         tbMemoryKind1 tbMemoryKind1
; CHECK:       endmodule

  module dutModule2:
    input clock: Clock
    input rAddr: UInt<4>
    input rEn: UInt<1>
    output rData: UInt<8>
    input wMask: UInt<1>
    input wData: UInt<8>

    mem dutMemory:
      data-type => UInt<8>
      depth => 32
      reader => r
      writer => w
      read-latency => 1
      write-latency => 1
      read-under-write => undefined

    dutMemory.r.clk <= clock
    dutMemory.r.en <= rEn
    dutMemory.r.addr <= rAddr
    rData <= dutMemory.r.data
    dutMemory.w.clk <= clock
    dutMemory.w.en <= rEn
    dutMemory.w.addr <= rAddr
    dutMemory.w.mask <= wMask
    dutMemory.w.data <= wData

; CHECK-LABEL: module dutModule2
; CHECK:  dutMemory dutMemory

  module hier1: 
    input clock: Clock
    input rAddr: UInt<4>
    input rEn: UInt<1>
    output rData: UInt<8>
    input wMask: UInt<1>
    input wData: UInt<8>

    mem tbMemoryKind1:
      data-type => UInt<8>
      depth => 16
      reader => r
      writer => w
      read-latency => 1
      write-latency => 1
      read-under-write => undefined

    tbMemoryKind1.r.clk <= clock
    tbMemoryKind1.r.en <= rEn
    tbMemoryKind1.r.addr <= rAddr
    rData <= tbMemoryKind1.r.data
    tbMemoryKind1.w.clk <= clock
    tbMemoryKind1.w.en <= rEn
    tbMemoryKind1.w.addr <= rAddr
    tbMemoryKind1.w.mask <= wMask
    tbMemoryKind1.w.data <= wData

; CHECK=LABEL: module hier1
; CHECK:  tbMemoryKind1_0 tbMemoryKind1


  module hier2: 
    input clock: Clock
    input rAddr: UInt<4>
    input rEn: UInt<1>
    output rData: UInt<8>
    input wMask: UInt<1>
    input wData: UInt<8>


    inst m of tbMemModule1
    m.clock <= clock
    m.rAddr <= rAddr
    m.rEn <= rEn
    rData <= m.rData
    m.wMask <= wMask
    m.wData <= wData

  module test: 
    input clock: Clock
    input rAddr: UInt<4>
    input rEn: UInt<1>
    output rData: UInt<8>
    input wMask: UInt<1>
    input wData: UInt<8>


    inst h1 of hier1
    h1.clock <= clock
    h1.rAddr <= rAddr
    h1.rEn <= rEn
    rData <= h1.rData
    h1.wMask <= wMask
    h1.wData <= wData

    inst h2 of hier2
    h2.clock <= clock
    h2.rAddr <= rAddr
    h2.rEn <= rEn
    rData <= h2.rData
    h2.wMask <= wMask
    h2.wData <= wData

    inst m2 of dutModule
    m2.clock <= clock
    m2.rAddr <= rAddr
    m2.rEn <= rEn
    rData <= m2.rData
    m2.wMask <= wMask
    m2.wData <= wData

; CHECK-LABEL: module test
; CHECK: hier1 [[h1:.+]] (
; CHECK: hier2 [[h2:.+]] (
; CHECK: dutModule [[m2:.+]] (

  module dutModule:
    input clock: Clock
    input rAddr: UInt<4>
    input rEn: UInt<1>
    output rData: UInt<8>
    input wMask: UInt<1>
    input wData: UInt<8>


    inst m of dutModule2
    m.clock <= clock
    m.rAddr <= rAddr
    m.rEn <= rEn
    rData <= m.rData
    m.wMask <= wMask
    m.wData <= wData

; CHECK-LABEL: module dutModule
; CHECK: dutModule2 [[m:.+]] (


; CHECK-LABEL: external module tbMemoryKind1_ext
; CHECK-LABEL: external module dutMemory_ext
; CHECK-LABEL: external module tbMemoryKind1_0_ext


; CHECK-LABEL: FILE "metadata/tb_seq_mems.json"
; CHECK:      [
; CHECK-NEXT:   {
; CHECK-NEXT:     "module_name": "tbMemoryKind1_ext",
; CHECK-NEXT:     "depth": 16,
; CHECK-NEXT:     "width": 8,
; CHECK-NEXT:     "masked": false,
; CHECK-NEXT:     "read": true,
; CHECK-NEXT:     "write": true,
; CHECK-NEXT:     "readwrite": false,
; CHECK-NEXT:     "extra_ports": [],
; CHECK-NEXT:     "hierarchy": [
; CHECK-NEXT:       "test.h2.m.tbMemoryKind1.tbMemoryKind1_ext"
; CHECK-NEXT:     ]
; CHECK-NEXT:   },
; CHECK-NEXT:   {
; CHECK-NEXT:     "module_name": "tbMemoryKind1_0_ext",
; CHECK-NEXT:     "depth": 16,
; CHECK-NEXT:     "width": 8,
; CHECK-NEXT:     "masked": false,
; CHECK-NEXT:     "read": true,
; CHECK-NEXT:     "write": true,
; CHECK-NEXT:     "readwrite": false,
; CHECK-NEXT:     "extra_ports": [],
; CHECK-NEXT:     "hierarchy": [
; CHECK-NEXT:       "test.h1.tbMemoryKind1.tbMemoryKind1_0_ext"
; CHECK-NEXT:     ]
; CHECK-NEXT:   }
; CHECK-NEXT: ]


; CHECK-LABEL: FILE "metadata/seq_mems.json"
; CHECK:      [
; CHECK-NEXT:   {
; CHECK-NEXT:     "module_name": "dutMemory_ext",
; CHECK-NEXT:     "depth": 32,
; CHECK-NEXT:     "width": 8,
; CHECK-NEXT:     "masked": false,
; CHECK-NEXT:     "read": true,
; CHECK-NEXT:     "write": true,
; CHECK-NEXT:     "readwrite": false,
; CHECK-NEXT:     "extra_ports": [],
; CHECK-NEXT:     "hierarchy": [
; CHECK-NEXT:       "dutModule.m.dutMemory.dutMemory_ext"
; CHECK-NEXT:     ]
; CHECK-NEXT:   }
; CHECK-NEXT: ]


; CHECK-LABEL: FILE "metadata/dutModule.conf"
; CHECK: name dutMemory_ext depth 32 width 8 ports write,read
; CHECK: name tbMemoryKind1_ext depth 16 width 8 ports write,read
; CHECK: name tbMemoryKind1_0_ext depth 16 width 8 ports write,read
