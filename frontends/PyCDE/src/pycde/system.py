#  Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
#  See https://llvm.org/LICENSE.txt for license information.
#  SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

from pycde.devicedb import (EntityExtern, PlacementDB, PrimitiveDB,
                            PhysicalRegion)

from .module import _SpecializedModule
from .pycde_types import types
from .instance import Instance, InstanceHierarchyRoot

from circt.dialects import hw, msft

import mlir
import mlir.ir as ir
import mlir.passmanager

import circt
import circt.dialects.msft
import circt.support

from contextvars import ContextVar
import gc
import os
import sys
from typing import Callable, Dict, Set, Tuple

_current_system = ContextVar("current_pycde_system")


class System:
  """The 'System' contains the user's design and some private bookkeeping. On
  construction, specify a list of 'root' modules which you wish to generate.
  Upon generation, the design will be fleshed out and all the dependent modules
  will be created.

  'System' also has methods to run through the CIRCT lowering, output tcl, and
  output SystemVerilog."""

  __slots__ = [
      "mod", "top_modules", "name", "passed", "_old_system_token", "_op_cache",
      "_generate_queue", "_output_directory", "files", "_instance_roots",
      "_placedb"
  ]

  PASSES = """
    msft-lower-constructs, msft-lower-instances, {partition}
    lower-msft-to-hw{{verilog-file={verilog_file}}},
    lower-esi-to-physical, lower-esi-ports, lower-esi-to-hw,
    lower-seq-to-sv, hw.module(prettify-verilog), hw.module(hw-cleanup),
    msft-export-tcl{{tops={tops} tcl-file={tcl_file}}}
  """

  def __init__(self,
               top_modules: list,
               name: str = "PyCDESystem",
               output_directory: str = None):
    self.passed = False
    self.mod = ir.Module.create()
    self.top_modules = list(top_modules)
    self.name = name
    self._op_cache: _OpCache = _OpCache(self.mod)

    self._generate_queue = []
    self._instance_roots: dict[_SpecializedModule, InstanceHierarchyRoot] = {}

    self._placedb: PlacementDB = None
    self.files: Set[str] = set()

    if output_directory is None:
      output_directory = os.path.join(os.getcwd(), self.name)
    self._output_directory = output_directory

    with self:
      [m._pycde_mod.create() for m in top_modules]

  def _get_ip(self):
    return ir.InsertionPoint(self.mod.body)

  @staticmethod
  def set_debug():
    ir._GlobalDebug.flag = True

  def create_physical_region(self, name: str = None):
    with self._get_ip():
      physical_region = PhysicalRegion(name)
    return physical_region

  def create_entity_extern(self, tag: str, metadata=""):
    with self._get_ip():
      entity_extern = EntityExtern(tag, metadata)
    return entity_extern

  def _create_circt_mod(self, spec_mod: _SpecializedModule, create_cb):
    """Wrapper for a callback (which actually builds the CIRCT op) which
    controls all the bookkeeping around CIRCT module ops."""

    (symbol, install_func) = self._op_cache.create_symbol(spec_mod)
    if symbol is None:
      return

    # Build the correct op.
    op = create_cb(symbol)
    # Install the op in the cache.
    install_func(op)
    # Add to the generation queue, if necessary.
    if isinstance(op, circt.dialects.msft.MSFTModuleOp):
      self._generate_queue.append(spec_mod)
      file_name = spec_mod.modcls.__name__ + ".sv"
      self.files.add(os.path.join(self._output_directory, file_name))
      op.fileName = ir.StringAttr.get(file_name)

  @staticmethod
  def current():
    """Get the top-most system in the stack created by `with System()`."""
    bb = _current_system.get(None)
    if bb is None:
      raise RuntimeError("No PyCDE system currently active!")
    return bb

  def __enter__(self):
    self._old_system_token = _current_system.set(self)

  def __exit__(self, exc_type, exc_value, traceback):
    if exc_value is not None:
      return
    _current_system.reset(self._old_system_token)

  @property
  def body(self):
    return self.mod.body

  def _passes(self, partition):
    tops = ",".join(
        [self._op_cache.get_module_symbol(m) for m in self.top_modules])
    verilog_file = self.name + ".sv"
    tcl_file = self.name + ".tcl"
    self.files.add(os.path.join(self._output_directory, verilog_file))
    self.files.add(os.path.join(self._output_directory, tcl_file))
    partition_str = "msft-partition," if partition else ""
    return self.PASSES.format(tops=tops,
                              partition=partition_str,
                              verilog_file=verilog_file,
                              tcl_file=tcl_file).strip()

  def print(self, *argv, **kwargs):
    self.mod.operation.print(*argv, **kwargs)

  def graph(self, short_names=True):
    import mlir.all_passes_registration
    pm = mlir.passmanager.PassManager.parse("view-op-graph{short-names=" +
                                            ("1" if short_names else "0") + "}")
    pm.run(self.mod)

  def cleanup(self):
    pm = mlir.passmanager.PassManager.parse("canonicalize")
    pm.run(self.mod)

  def generate(self, generator_names=[], iters=None):
    """Fully generate the system unless iters is specified. Iters specifies the
    number of generators to run. Useful for debugging. Maybe."""
    i = 0
    with self:
      while len(self._generate_queue) > 0 and (iters is None or i < iters):
        m = self._generate_queue.pop()
        m.generate()
        i += 1
    return len(self._generate_queue)

  def get_instance(self, mod_cls: object) -> InstanceHierarchyRoot:
    mod = mod_cls._pycde_mod
    if mod not in self._instance_roots:
      self._instance_roots[mod] = InstanceHierarchyRoot(mod, self)
    return self._instance_roots[mod]

  def run_passes(self, partition=False):
    if self.passed:
      return
    if len(self._generate_queue) > 0:
      print("WARNING: running lowering passes on partially generated design!",
            file=sys.stderr)

    # By now, we have all the types defined so we can go through and output the
    # typedefs delcarations.
    types.declare_types(self.mod)
    self._op_cache.release_ops()

    pm = mlir.passmanager.PassManager.parse(self._passes(partition))
    pm.run(self.mod)
    self.passed = True

  def emit_outputs(self):
    self.run_passes()
    circt.export_split_verilog(self.mod, self._output_directory)

  @property
  def placedb(self):
    if self._placedb is None:
      raise Exception("Must `createdb` first")
    return self._placedb

  def createdb(self, primdb: PrimitiveDB = None):
    if self._placedb is None:
      self._placedb = PlacementDB(self, self.mod, primdb)


class _OpCache:
  """Used to cache CIRCT operations and handle symbols."""

  import pycde.instance as pi

  __slots__ = [
      "_module", "_symbols", "_module_symbols", "_symbol_modules",
      "_instance_hier_cache", "_instance_hier_obj_cache", "_instance_cache",
      "_module_inside_sym_cache", "_dyn_insts_in_inst"
  ]

  def __init__(self, module: ir.Module):
    self._module = module
    self._symbols: Dict[str, ir.OpView] = None
    self._module_symbols: dict[_SpecializedModule, str] = {}
    self._symbol_modules: dict[str, _SpecializedModule] = {}

    self._instance_hier_cache: dict[str, msft.InstanceHierarchyOp] = None
    self._instance_hier_obj_cache: dict[str, InstanceHierarchyRoot] = {}
    self._instance_cache: dict[Instance, msft.DynamicInstanceOp] = {}

    self._module_inside_sym_cache: Dict[ir.Operation, Dict[ir.Attribute,
                                                           ir.Operation]] = {}
    self._dyn_insts_in_inst: Dict[ir.Operation,
                                  Dict[ir.Attribute,
                                       msft.DynamicInstanceOp]] = {}

  def release_ops(self):
    """Clear all of the MLIR ops we store. Call this before each transition to
    MLIR C++."""
    self._symbols = None
    self._instance_hier_cache = None
    self._instance_cache.clear()
    self._module_inside_sym_cache.clear()
    self._dyn_insts_in_inst.clear()
    gc.collect()
    num_ops_live = ir.Context.current._clear_live_operations()
    if num_ops_live > 0:
      sys.stderr.write(
          f"Warning: something is holding references to {num_ops_live} MLIR ops\n"
      )

  @property
  def symbols(self):
    if self._symbols is None:
      self._symbols = {}
      for op in self._module.operation.regions[0].blocks[0]:
        if "sym_name" in op.attributes:
          self._symbols[ir.StringAttr(op.attributes["sym_name"]).value] = op
    return self._symbols

  def op(self, symbol: str) -> ir.OpView:
    """Resolve a symbol to an op."""
    return self.symbols[symbol]

  def create_symbol(self, spec_mod: _SpecializedModule) -> Tuple[str, Callable]:
    """Create a unique symbol and add it to the cache. If it is to be preserved,
    the caller must use it as the symbol on a top-level op. Returns the symbol
    string and a callback to install the mapping. Return (None, None) if
    `spec_mod` already has a symbol."""

    if spec_mod in self._module_symbols:
      return (None, None)
    ctr = 0
    basename = spec_mod.name
    symbol = basename
    while symbol in self.symbols:
      ctr += 1
      symbol = basename + "_" + str(ctr)

    def install(op):
      self._symbols[symbol] = op
      self._module_symbols[spec_mod] = symbol
      self._symbol_modules[symbol] = spec_mod

    return symbol, install

  def get_symbol_module(self, symbol):
    """Get the _SpecializedModule for a symbol."""
    if isinstance(symbol, ir.FlatSymbolRefAttr):
      symbol = symbol.value
    return self._symbol_modules[symbol]

  def get_module_symbol(self, spec_mod) -> str:
    """Get the symbol for a module or its associated _SpecializedModule."""
    if not isinstance(spec_mod, _SpecializedModule):
      if not hasattr(spec_mod, "_pycde_mod"):
        raise TypeError("Expected _SpecializedModule or pycde module")
      spec_mod = spec_mod._pycde_mod
    if spec_mod not in self._module_symbols:
      return None
    return self._module_symbols[spec_mod]

  def get_circt_mod(self, spec_mod: _SpecializedModule) -> ir.Operation:
    """Get the CIRCT module op for a PyCDE module."""
    return self.symbols[self.get_module_symbol(spec_mod)]

  def _build_instance_hier_cache(self):
    """If the instance hierarchy cache doesn't exist, build it."""
    if self._instance_hier_cache is None:
      self._instance_hier_cache = {}
      for op in self._module.operation.regions[0].blocks[0]:
        if isinstance(op, msft.InstanceHierarchyOp):
          self._instance_hier_cache[op.top_module_ref] = op

  def create_instance_hier_op(
      self, inst_hier: InstanceHierarchyRoot) -> msft.InstanceHierarchyOp:
    """Create an instance hierarchy op 'inst_hier' and add it to the cache.
    Assert if one already exists in the cache."""

    root_mod_symbol = ir.FlatSymbolRefAttr.get(
        self.get_module_symbol(inst_hier.inside_of))

    self._build_instance_hier_cache()
    assert root_mod_symbol not in self._instance_hier_cache, \
      "Cannot create two instance hierarchy roots for same module"

    with ir.InsertionPoint(self._module.body):
      hier_op = msft.InstanceHierarchyOp.create(root_mod_symbol)
      self._instance_hier_cache[root_mod_symbol] = hier_op
      self._instance_hier_obj_cache[root_mod_symbol] = inst_hier

    return hier_op

  def get_instance_hier_op(
      self, inst_hier: InstanceHierarchyRoot) -> msft.InstanceHierarchyOp:
    """Lookup an instance hierarchy op in the cache. None if not found."""

    self._build_instance_hier_cache()
    root_mod_symbol = ir.FlatSymbolRefAttr.get(
        self.get_module_symbol(inst_hier.inside_of))
    return self._instance_hier_cache.get(root_mod_symbol, None)

  def create_or_get_dyn_inst(self, inst: Instance) -> msft.DynamicInstanceOp:
    """Get the dynamic instance op corresponding to 'inst'. Returns 'None' if
    the instance doesn't have a static op in the IR."""

    # Check static op existence.
    inside_of_syms = self.get_sym_ops_in_module(inst.inside_of)
    if inst.symbol not in inside_of_syms:
      return None

    if inst not in self._instance_cache:
      ref = hw.InnerRefAttr.get(ir.StringAttr.get(inst._inside_of_symbol),
                                inst.symbol)
      # Check if the dynamic instance op exists.
      parent_op = inst.parent._dyn_inst
      insts_in_parent = self.get_dyn_insts_in_inst(parent_op)
      if ref in insts_in_parent:
        # If it is, install it into the instance cache
        inst_op = insts_in_parent[ref]
        self._instance_cache[inst] = inst_op
      else:
        # If not, create a new one and install it into the instance cache and
        # add it to the insts in parent cache.
        with inst.parent._get_ip():
          new_inst = msft.DynamicInstanceOp.create(ref)
          self._instance_cache[inst] = new_inst
          insts_in_parent[ref] = new_inst

    return self._instance_cache[inst]

  def get_or_create_inst_from_op(self, op: ir.OpView) -> pi.Instance:
    """Descend the Python instance hierarchy from the CIRCT IR, returning the
    Python Instance corresponding to 'op'."""

    if isinstance(op, msft.InstanceHierarchyOp):
      return self._instance_hier_obj_cache[op.top_module_ref]
    if isinstance(op, msft.DynamicInstanceOp):
      parent_inst = self.get_or_create_inst_from_op(op.operation.parent.opview)
      instance_ref = hw.InnerRefAttr(op.instanceRef)
      return parent_inst._children()[instance_ref.name]
    raise TypeError(
        "Can only resolve from InstanceHierarchyOp or DynamicInstanceOp")

  def get_sym_ops_in_module(
      self, module: _SpecializedModule) -> Dict[ir.Attribute, ir.Operation]:
    """Look into the IR inside 'module' for any ops which have a `sym_name`
    attribute. Cached."""

    if module is None:
      return {}
    circt_mod = self.get_circt_mod(module)
    if isinstance(circt_mod, msft.MSFTModuleExternOp):
      return {}

    if circt_mod not in self._module_inside_sym_cache:
      self._module_inside_sym_cache[circt_mod] = \
        {op.attributes["sym_name"]: op
         for op in circt_mod.entry_block
         if "sym_name" in op.attributes}

    return self._module_inside_sym_cache[circt_mod]

  def get_dyn_insts_in_inst(
      self, inst: ir.Operation) -> Dict[ir.Attribute, msft.DynamicInstanceOp]:
    """Get a mapping of existing dynamic instances inside of instance-like
    'inst'."""

    if inst not in self._dyn_insts_in_inst:
      self._dyn_insts_in_inst[inst] = \
        {op.instanceRef: op for op in inst.body.blocks[0]
         if isinstance(op, msft.DynamicInstanceOp)}
    return self._dyn_insts_in_inst[inst]
