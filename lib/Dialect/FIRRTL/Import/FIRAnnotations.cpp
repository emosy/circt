//===- FIRAnnotations.cpp - FIRRTL Annotation Utilities -------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Provide utilities related to dealing with FIRRTL Annotations.
//
//===----------------------------------------------------------------------===//

#include "FIRAnnotations.h"

#include "circt/Dialect/FIRRTL/AnnotationDetails.h"
#include "circt/Dialect/FIRRTL/FIRParser.h"
#include "circt/Dialect/FIRRTL/FIRRTLAnnotationHelper.h"
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include "circt/Dialect/HW/HWAttributes.h"
#include "mlir/IR/BuiltinAttributes.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/Diagnostics.h"
#include "mlir/IR/OperationSupport.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/ADT/TypeSwitch.h"
#include "llvm/Support/JSON.h"

namespace json = llvm::json;

using namespace circt;
using namespace firrtl;
using mlir::UnitAttr;

/// Split a target into a base target (including a reference if one exists) and
/// an optional array of subfield/subindex tokens.
static std::pair<StringRef, llvm::Optional<ArrayAttr>>
splitTarget(StringRef target, MLIRContext *context) {
  if (target.empty())
    return {target, None};

  // Find the string index where the target can be partitioned into the "base
  // target" and the "target".  The "base target" is the module or instance and
  // the "target" is everything else.  This has two variants that need to be
  // considered:
  //
  //   1) A Local target, e.g., ~Foo|Foo>bar.baz
  //   2) An instance target, e.g., ~Foo|Foo/bar:Bar>baz.qux
  //
  // In (1), this should be partitioned into ["~Foo|Foo>bar", ".baz"].  In (2),
  // this should be partitioned into ["~Foo|Foo/bar:Bar", ">baz.qux"].
  bool isInstance = false;
  size_t fieldBegin = target.find_if_not([&isInstance](char c) {
    switch (c) {
    case '/':
      return isInstance = true;
    case '>':
      return !isInstance;
    case '[':
    case '.':
      return false;
    default:
      return true;
    };
  });

  // Exit if the target does not contain a subfield or subindex.
  if (fieldBegin == StringRef::npos)
    return {target, None};

  auto targetBase = target.take_front(fieldBegin);
  target = target.substr(fieldBegin);
  SmallVector<Attribute> annotationVec;
  SmallString<16> temp;
  for (auto c : target.drop_front()) {
    if (c == ']') {
      // Create a IntegerAttr with the previous sub-index token.
      APInt subIndex;
      if (!temp.str().getAsInteger(10, subIndex))
        annotationVec.push_back(IntegerAttr::get(IntegerType::get(context, 64),
                                                 subIndex.getZExtValue()));
      else
        // We don't have a good way to emit error here. This will be reported as
        // an error in the FIRRTL parser.
        annotationVec.push_back(StringAttr::get(context, temp));
      temp.clear();
    } else if (c == '[' || c == '.') {
      // Create a StringAttr with the previous token.
      if (!temp.empty())
        annotationVec.push_back(StringAttr::get(context, temp));
      temp.clear();
    } else
      temp.push_back(c);
  }
  // Save the last token.
  if (!temp.empty())
    annotationVec.push_back(StringAttr::get(context, temp));

  return {targetBase, ArrayAttr::get(context, annotationVec)};
}

/// Split out non-local paths.  This will return a set of target strings for
/// each named entity along the path.
/// c|c:ai/Am:bi/Bm>d.agg[3] ->
/// c|c>ai, c|Am>bi, c|Bm>d.agg[2]
static SmallVector<std::tuple<std::string, StringRef, StringRef>>
expandNonLocal(StringRef target) {
  SmallVector<std::tuple<std::string, StringRef, StringRef>> retval;
  StringRef circuit;
  std::tie(circuit, target) = target.split('|');
  while (target.count(':')) {
    StringRef nla;
    std::tie(nla, target) = target.split(':');
    StringRef inst, mod;
    std::tie(mod, inst) = nla.split('/');
    retval.emplace_back((circuit + "|" + mod + ">" + inst).str(), mod, inst);
  }
  if (target.empty()) {
    retval.emplace_back(circuit.str(), "", "");
  } else {

    StringRef mod, name;
    // remove aggregate
    auto targetBase =
        target.take_until([](char c) { return c == '.' || c == '['; });
    std::tie(mod, name) = targetBase.split('>');
    retval.emplace_back((circuit + "|" + target).str(), mod, name);
  }
  return retval;
}

/// Make an anchor for a non-local annotation.  Use the expanded path to build
/// the module and name list in the anchor.
static FlatSymbolRefAttr
buildNLA(CircuitOp circuit, size_t nlaSuffix,
         SmallVectorImpl<std::tuple<std::string, StringRef, StringRef>> &nlas) {
  OpBuilder b(circuit.getBodyRegion());
  MLIRContext *ctxt = circuit.getContext();
  SmallVector<Attribute> insts;
  for (auto &nla : nlas) {
    // Assumption: Symbol name = Operation name.
    auto module = std::get<1>(nla);
    auto inst = std::get<2>(nla);
    if (inst.empty())
      insts.push_back(FlatSymbolRefAttr::get(ctxt, module));
    else
      insts.push_back(hw::InnerRefAttr::get(StringAttr::get(ctxt, module),
                                            StringAttr::get(ctxt, inst)));
  }
  auto instAttr = ArrayAttr::get(ctxt, insts);
  auto nla = b.create<HierPathOp>(circuit.getLoc(),
                                  "nla_" + std::to_string(nlaSuffix), instAttr);
  return FlatSymbolRefAttr::get(nla);
}

/// Append the argument `target` to the `annotation` using the key "target".
static inline void appendTarget(NamedAttrList &annotation, ArrayAttr target) {
  annotation.append("target", target);
}

/// Mutably update a prototype Annotation (stored as a `NamedAttrList`) with
/// subfield/subindex information from a Target string.  Subfield/subindex
/// information will be placed in the key "target" at the back of the
/// Annotation.  If no subfield/subindex information, the Annotation is
/// unmodified.  Return the split input target as a base target (include a
/// reference if one exists) and an optional array containing subfield/subindex
/// tokens.
static std::pair<StringRef, llvm::Optional<ArrayAttr>>
splitAndAppendTarget(NamedAttrList &annotation, StringRef target,
                     MLIRContext *context) {
  auto targetPair = splitTarget(target, context);
  if (targetPair.second.hasValue())
    appendTarget(annotation, targetPair.second.getValue());

  return targetPair;
}

/// Return an input \p target string in canonical form.  This converts a Legacy
/// Annotation (e.g., A.B.C) into a modern annotation (e.g., ~A|B>C).  Trailing
/// subfield/subindex references are preserved.
static llvm::Optional<std::string> oldCanonicalizeTarget(StringRef target) {

  if (target.empty())
    return {};

  // If this is a normal Target (not a Named), erase that field in the JSON
  // object and return that Target.
  if (target[0] == '~')
    return target.str();

  // This is a legacy target using the firrtl.annotations.Named type.  This
  // can be trivially canonicalized to a non-legacy target, so we do it with
  // the following three mappings:
  //   1. CircuitName => CircuitTarget, e.g., A -> ~A
  //   2. ModuleName => ModuleTarget, e.g., A.B -> ~A|B
  //   3. ComponentName => ReferenceTarget, e.g., A.B.C -> ~A|B>C
  std::string newTarget = "~";
  llvm::raw_string_ostream s(newTarget);
  unsigned tokenIdx = 0;
  for (auto a : target) {
    if (a == '.') {
      switch (tokenIdx) {
      case 0:
        s << "|";
        break;
      case 1:
        s << ">";
        break;
      default:
        s << ".";
        break;
      }
      ++tokenIdx;
    } else
      s << a;
  }
  return llvm::Optional<std::string>(newTarget);
}

/// Convert arbitrary JSON to an MLIR Attribute.
static Attribute convertJSONToAttribute(MLIRContext *context,
                                        json::Value &value, json::Path p) {
  // String or quoted JSON
  if (auto a = value.getAsString()) {
    // Test to see if this might be quoted JSON (a string that is actually
    // JSON).  Sometimes FIRRTL developers will do this to serialize objects
    // that the Scala FIRRTL Compiler doesn't know about.
    auto unquotedValue = json::parse(a.getValue());
    auto err = unquotedValue.takeError();
    // If this parsed without an error, then it's more JSON and recurse on
    // that.
    if (!err)
      return convertJSONToAttribute(context, unquotedValue.get(), p);
    // If there was an error, then swallow it and handle this as a string.
    handleAllErrors(std::move(err), [&](const json::ParseError &a) {});
    return StringAttr::get(context, a.getValue());
  }

  // Integer
  if (auto a = value.getAsInteger())
    return IntegerAttr::get(IntegerType::get(context, 64), a.getValue());

  // Float
  if (auto a = value.getAsNumber())
    return FloatAttr::get(mlir::FloatType::getF64(context), a.getValue());

  // Boolean
  if (auto a = value.getAsBoolean())
    return BoolAttr::get(context, a.getValue());

  // Null
  if (auto a = value.getAsNull())
    return mlir::UnitAttr::get(context);

  // Object
  if (auto a = value.getAsObject()) {
    NamedAttrList metadata;
    for (auto b : *a)
      metadata.append(
          b.first, convertJSONToAttribute(context, b.second, p.field(b.first)));
    return DictionaryAttr::get(context, metadata);
  }

  // Array
  if (auto a = value.getAsArray()) {
    SmallVector<Attribute> metadata;
    for (size_t i = 0, e = (*a).size(); i != e; ++i)
      metadata.push_back(convertJSONToAttribute(context, (*a)[i], p.index(i)));
    return ArrayAttr::get(context, metadata);
  }

  llvm_unreachable("Impossible unhandled JSON type");
}

static std::string addNLATargets(
    MLIRContext *context, StringRef targetStrRef, CircuitOp circuit,
    size_t &nlaNumber, NamedAttrList &metadata,
    llvm::StringMap<llvm::SmallVector<Attribute>> &mutableAnnotationMap) {

  auto nlaTargets = expandNonLocal(targetStrRef);

  FlatSymbolRefAttr nlaSym;
  if (nlaTargets.size() > 1) {
    nlaSym = buildNLA(circuit, ++nlaNumber, nlaTargets);
    metadata.append("circt.nonlocal", nlaSym);
  }

  for (int i = 0, e = nlaTargets.size() - 1; i < e; ++i) {
    NamedAttrList dontTouch;
    dontTouch.append(
        "class",
        StringAttr::get(context, "firrtl.transforms.DontTouchAnnotation"));
    // Every op with nonlocal anchor must have a symbol. Hence add the
    // dontTouch.
    mutableAnnotationMap[std::get<0>(nlaTargets[i])].push_back(
        DictionaryAttr::get(context, dontTouch));
  }

  // Annotations on the element instance.
  auto leafTarget =
      splitAndAppendTarget(metadata, std::get<0>(nlaTargets.back()), context)
          .first;

  return leafTarget.str();
}

/// Deserialize a JSON value into FIRRTL Annotations.  Annotations are
/// represented as a Target-keyed arrays of attributes.  The input JSON value is
/// checked, at runtime, to be an array of objects.  Returns true if successful,
/// false if unsuccessful.
bool circt::firrtl::fromJSON(json::Value &value, StringRef circuitTarget,
                             llvm::StringMap<ArrayAttr> &annotationMap,
                             json::Path path, CircuitOp circuit,
                             size_t &nlaNumber) {
  auto context = circuit.getContext();

  /// Examine an Annotation JSON object and return an optional string indicating
  /// the target associated with this annotation.  Erase the target from the
  /// JSON object if a target was found.  Automatically convert any legacy Named
  /// targets to actual Targets.  Note: it is expected that a target may not
  /// exist, e.g., any subclass of firrtl.annotations.NoTargetAnnotation will
  /// not have a target.
  auto findAndEraseTarget = [](json::Object *object,
                               json::Path p) -> llvm::Optional<std::string> {
    // If no "target" field exists, then promote the annotation to a
    // CircuitTarget annotation by returning a target of "~".
    auto maybeTarget = object->get("target");
    if (!maybeTarget)
      return llvm::Optional<std::string>("~");

    // Find the target.
    auto maybeTargetStr = maybeTarget->getAsString();
    if (!maybeTargetStr) {
      p.field("target").report("target must be a string type");
      return {};
    }
    auto canonTargetStr = oldCanonicalizeTarget(maybeTargetStr.getValue());
    if (!canonTargetStr) {
      p.field("target").report("invalid target string");
      return {};
    }

    auto target = canonTargetStr.getValue();

    // Remove the target field from the annotation and return the target.
    object->erase("target");
    return llvm::Optional<std::string>(target);
  };

  // The JSON value must be an array of objects.  Anything else is reported as
  // invalid.
  auto array = value.getAsArray();
  if (!array) {
    path.report(
        "Expected annotations to be an array, but found something else.");
    return false;
  }

  // Build a mutable map of Target to Annotation.
  llvm::StringMap<llvm::SmallVector<Attribute>> mutableAnnotationMap;
  for (size_t i = 0, e = (*array).size(); i != e; ++i) {
    auto object = (*array)[i].getAsObject();
    auto p = path.index(i);
    if (!object) {
      p.report("Expected annotations to be an array of objects, but found an "
               "array of something else.");
      return false;
    }

    // If the annotation has a class name which matches an annotation which the
    // LowerAnnotations pass knows about, then defer its processing.
    if (auto *clazz = object->get("class")) {
      auto classString = clazz->getAsString();
      if (classString && isAnnoClassLowered(classString.getValue())) {
        NamedAttrList metadata;
        for (auto field : *object) {
          if (auto value = convertJSONToAttribute(context, field.second, p)) {
            metadata.append(field.first, value);
            continue;
          }
          return false;
        }
        mutableAnnotationMap[rawAnnotations].push_back(
            DictionaryAttr::get(context, metadata));
        continue;
      }
    }

    // Find and remove the "target" field from the Annotation object if it
    // exists.  In the FIRRTL Dialect, the target will be implicitly specified
    // based on where the attribute is applied.
    auto optTarget = findAndEraseTarget(object, p);
    if (!optTarget)
      return false;
    StringRef targetStrRef = optTarget.getValue();

    if (targetStrRef != "~") {
      auto circuitFieldEnd = targetStrRef.find_first_of('|');
      if (circuitTarget != targetStrRef.take_front(circuitFieldEnd)) {
        p.report("annotation has invalid circuit name");
        return false;
      }
    }

    // Build up the Attribute to represent the Annotation and store it in the
    // global Target -> Attribute mapping.
    NamedAttrList metadata;
    for (auto field : *object) {
      if (auto value = convertJSONToAttribute(context, field.second, p)) {
        metadata.append(field.first, value);
        continue;
      }
      return false;
    }

    auto leafTarget = addNLATargets(context, targetStrRef, circuit, nlaNumber,
                                    metadata, mutableAnnotationMap);

    mutableAnnotationMap[leafTarget].push_back(
        DictionaryAttr::get(context, metadata));
  }

  // Convert the mutable Annotation map to a SmallVector<ArrayAttr>.
  for (auto a : mutableAnnotationMap.keys()) {
    // If multiple annotations on a single object, then append it.
    if (annotationMap.count(a))
      for (auto attr : annotationMap[a])
        mutableAnnotationMap[a].push_back(attr);

    annotationMap[a] = ArrayAttr::get(context, mutableAnnotationMap[a]);
  }

  return true;
}

/// Convert a JSON value containing OMIR JSON (an array of OMNodes), convert
/// this to an OMIRAnnotation, and add it to a mutable `annotationMap` argument.
bool circt::firrtl::fromOMIRJSON(json::Value &value, StringRef circuitTarget,
                                 llvm::StringMap<ArrayAttr> &annotationMap,
                                 json::Path path, MLIRContext *context) {
  // The JSON value must be an array of objects.  Anything else is reported as
  // invalid.
  auto *array = value.getAsArray();
  if (!array) {
    path.report(
        "Expected OMIR to be an array of nodes, but found something else.");
    return false;
  }

  // Build a mutable map of Target to Annotation.
  SmallVector<Attribute> omnodes;
  for (size_t i = 0, e = (*array).size(); i != e; ++i) {
    auto *object = (*array)[i].getAsObject();
    auto p = path.index(i);
    if (!object) {
      p.report("Expected OMIR to be an array of objects, but found an array of "
               "something else.");
      return false;
    }

    // Manually built up OMNode.
    NamedAttrList omnode;

    // Validate that this looks like an OMNode.  This should have three fields:
    //   - "info": String
    //   - "id": String that starts with "OMID:"
    //   - "fields": Array<Object>
    // Fields is optional and is a dictionary encoded as an array of objects:
    //   - "info": String
    //   - "name": String
    //   - "value": JSON
    // The dictionary is keyed by the "name" member and the array of fields is
    // guaranteed to not have collisions of the "name" key.
    auto maybeInfo = object->getString("info");
    if (!maybeInfo) {
      p.report("OMNode missing mandatory member \"info\" with type \"string\"");
      return false;
    }
    auto maybeID = object->getString("id");
    if (!maybeID || !maybeID.getValue().startswith("OMID:")) {
      p.report("OMNode missing mandatory member \"id\" with type \"string\" "
               "that starts with \"OMID:\"");
      return false;
    }
    auto *maybeFields = object->get("fields");
    if (maybeFields && !maybeFields->getAsArray()) {
      p.report("OMNode has \"fields\" member with incorrect type (expected "
               "\"array\")");
      return false;
    }
    Attribute fields;
    if (!maybeFields)
      fields = DictionaryAttr::get(context, {});
    else {
      auto array = *maybeFields->getAsArray();
      NamedAttrList fieldAttrs;
      for (size_t i = 0, e = array.size(); i != e; ++i) {
        auto *field = array[i].getAsObject();
        auto pI = p.field("fields").index(i);
        if (!field) {
          pI.report("OMNode has field that is not an \"object\"");
          return false;
        }
        auto maybeInfo = field->getString("info");
        if (!maybeInfo) {
          pI.report(
              "OMField missing mandatory member \"info\" with type \"string\"");
          return false;
        }
        auto maybeName = field->getString("name");
        if (!maybeName) {
          pI.report(
              "OMField missing mandatory member \"name\" with type \"string\"");
          return false;
        }
        auto *maybeValue = field->get("value");
        if (!maybeValue) {
          pI.report("OMField missing mandatory member \"value\"");
          return false;
        }
        NamedAttrList values;
        values.append("info", StringAttr::get(context, maybeInfo.getValue()));
        values.append("value", convertJSONToAttribute(context, *maybeValue,
                                                      pI.field("value")));
        fieldAttrs.append(maybeName.getValue(),
                          DictionaryAttr::get(context, values));
      }
      fields = DictionaryAttr::get(context, fieldAttrs);
    }

    omnode.append("info", StringAttr::get(context, maybeInfo.getValue()));
    omnode.append("id", convertJSONToAttribute(context, *object->get("id"),
                                               p.field("id")));
    omnode.append("fields", fields);
    omnodes.push_back(DictionaryAttr::get(context, omnode));
  }

  NamedAttrList omirAnnoFields;
  omirAnnoFields.append("class", StringAttr::get(context, omirAnnoClass));
  omirAnnoFields.append("nodes", convertJSONToAttribute(context, value, path));

  DictionaryAttr omirAnno = DictionaryAttr::get(context, omirAnnoFields);

  // If no circuit annotations exist, just insert the OMIRAnnotation.
  auto &oldAnnotations = annotationMap["~"];
  if (!oldAnnotations) {
    oldAnnotations = ArrayAttr::get(context, {omirAnno});
    return true;
  }

  // Rewrite the ArrayAttr for the circuit.
  SmallVector<Attribute> newAnnotations(oldAnnotations.begin(),
                                        oldAnnotations.end());
  newAnnotations.push_back(omirAnno);
  oldAnnotations = ArrayAttr::get(context, newAnnotations);

  return true;
}

/// Recursively walk Object Model IR and convert FIRRTL targets to identifiers
/// while scattering trackers into the newAnnotations argument.
///
/// Object Model IR consists of a type hierarchy built around recursive arrays
/// and dictionaries whose leaves are "string-encoded types".  This is an Object
/// Model-specific construct that puts type information alongside a value.
/// Concretely, these look like:
///
///     'OM' type ':' value
///
/// This function is only concerned with unpacking types whose values are FIRRTL
/// targets.  This is because these need to be kept up-to-date with
/// modifications made to the circuit whereas other types are just passing
/// through CIRCT.
///
/// At a later time this understanding may be expanded or Object Model IR may
/// become its own Dialect.  At this time, this function is trying to do as
/// minimal work as possible to just validate that the OMIR looks okay without
/// doing lots of unnecessary unpacking/repacking of string-encoded types.
static Optional<Attribute>
scatterOMIR(Attribute original, unsigned &annotationID,
            llvm::StringMap<llvm::SmallVector<Attribute>> &newAnnotations,
            CircuitOp circuit, size_t &nlaNumber) {
  auto *ctx = original.getContext();

  // Convert a string-encoded type to a dictionary that includes the type
  // information and an identifier derived from the current annotationID.  Then
  // increment the annotationID.  Return the constructed dictionary.
  auto addID = [&](StringRef tpe, StringRef path) -> DictionaryAttr {
    NamedAttrList fields;
    fields.append("id",
                  IntegerAttr::get(IntegerType::get(ctx, 64), annotationID++));
    fields.append("omir.tracker", UnitAttr::get(ctx));
    fields.append("path", StringAttr::get(ctx, path));
    fields.append("type", StringAttr::get(ctx, tpe));
    return DictionaryAttr::getWithSorted(ctx, fields);
  };

  return TypeSwitch<Attribute, Optional<Attribute>>(original)
      // Most strings in the Object Model are actually string-encoded types.
      // These are types which look like: "<type>:<value>".  This code will
      // examine all strings, parse them into type and value, and then either
      // store them in their unpacked state (and possibly scatter trackers into
      // the circuit), store them in their packed state (because CIRCT is not
      // expected to care about them right now), or error if we see them
      // (because they should not exist and are expected to serialize to a
      // different format).
      .Case<StringAttr>([&](StringAttr str) -> Optional<Attribute> {
        // Unpack the string into type and value.
        StringRef tpe, value;
        std::tie(tpe, value) = str.getValue().split(":");

        // These are string-encoded types that are targets in the circuit.
        // These require annotations to be scattered for them.  Replace their
        // target with an ID and scatter a tracker.
        if (tpe == "OMReferenceTarget" || tpe == "OMMemberReferenceTarget" ||
            tpe == "OMMemberInstanceTarget" || tpe == "OMInstanceTarget" ||
            tpe == "OMDontTouchedReferenceTarget") {
          NamedAttrList tracker;
          tracker.append("class", StringAttr::get(ctx, omirTrackerAnnoClass));
          tracker.append(
              "id", IntegerAttr::get(IntegerType::get(ctx, 64), annotationID));

          auto canonTarget = oldCanonicalizeTarget(value);
          if (!canonTarget)
            return None;

          auto leafTarget = addNLATargets(ctx, *canonTarget, circuit, nlaNumber,
                                          tracker, newAnnotations);

          newAnnotations[leafTarget].push_back(
              DictionaryAttr::get(ctx, tracker));

          return addID(tpe, value);
        }

        // The following are types that may exist, but we do not unbox them.  At
        // a later time, we may want to change this behavior and unbox these if
        // we wind up building out an Object Model dialect:
        if (isOMIRStringEncodedPassthrough(tpe))
          return str;

        // The following types are not expected to exist because they have
        // serializations to JSON types or are removed during serialization.
        // Hence, any of the following types are NOT expected to exist and we
        // error if we see them.  These are explicitly specified as opposed to
        // being handled in the "unknown" catch-all case below because we want
        // to provide a good error message that a user may be doing something
        // very weird.
        if (tpe == "OMMap" || tpe == "OMArray" || tpe == "OMBoolean" ||
            tpe == "OMInt" || tpe == "OMDouble" || tpe == "OMFrozenTarget") {
          auto diag =
              mlir::emitError(circuit.getLoc())
              << "found known string-encoded OMIR type \"" << tpe
              << "\", but this type should not be seen as it has a defined "
                 "serialization format that does NOT use a string-encoded type";
          diag.attachNote()
              << "the problematic OMIR is reproduced here: " << original;
          return None;
        }

        // This is a catch-all for any unknown types.
        auto diag = mlir::emitError(circuit.getLoc())
                    << "found unknown string-encoded OMIR type \"" << tpe
                    << "\" (Did you misspell it?  Is CIRCT missing an Object "
                       "Model OMIR type?)";
        diag.attachNote() << "the problematic OMIR is reproduced here: "
                          << original;
        return None;
      })
      // For an array, just recurse into each element and rewrite the array with
      // the results.
      .Case<ArrayAttr>([&](ArrayAttr arr) -> Optional<Attribute> {
        SmallVector<Attribute> newArr;
        for (auto element : arr) {
          auto newElement = scatterOMIR(element, annotationID, newAnnotations,
                                        circuit, nlaNumber);
          if (!newElement)
            return None;
          newArr.push_back(newElement.getValue());
        }
        return ArrayAttr::get(ctx, newArr);
      })
      // For a dictionary, recurse into each value and rewrite the key/value
      // pairs.
      .Case<DictionaryAttr>([&](DictionaryAttr dict) -> Optional<Attribute> {
        NamedAttrList newAttrs;
        for (auto pairs : dict) {
          auto maybeValue = scatterOMIR(pairs.getValue(), annotationID,
                                        newAnnotations, circuit, nlaNumber);
          if (!maybeValue)
            return None;
          newAttrs.append(pairs.getName(), maybeValue.getValue());
        }
        return DictionaryAttr::get(ctx, newAttrs);
      })
      // These attributes are all expected.  They are OMIR types, but do not
      // have string-encodings (hence why these should error if we see them as
      // strings).
      .Case</* OMBoolean */ BoolAttr, /* OMDouble */ FloatAttr,
            /* OMInt */ IntegerAttr>(
          [](auto passThrough) { return passThrough; })
      // Error if we see anything else.
      .Default([&](auto) -> Optional<Attribute> {
        auto diag = mlir::emitError(circuit.getLoc())
                    << "found unexpected MLIR attribute \"" << original
                    << "\" while trying to scatter OMIR";
        return None;
      });
}

/// Convert an Object Model Field into an optional pair of a string key and a
/// dictionary attribute.  Expand internal source locator strings to location
/// attributes.  Scatter any FIRRTL targets into the circuit. If this is an
/// illegal Object Model Field return None.
///
/// Each Object Model Field consists of three mandatory members with
/// the following names and types:
///
///   - "info": Source Locator String
///   - "name": String
///   - "value": Object Model IR
///
/// The key is the "name" and the dictionary consists of the "info" and "value"
/// members.  Each value is recursively traversed to scatter any FIRRTL targets
/// that may be used inside it.
///
/// This conversion from an object (dictionary) to key--value pair is safe
/// because each Object Model Field in an Object Model Node must have a unique
/// "name".  Anything else is illegal Object Model.
static Optional<std::pair<StringRef, DictionaryAttr>>
scatterOMField(Attribute original, const Attribute root, unsigned &annotationID,
               llvm::StringMap<llvm::SmallVector<Attribute>> &newAnnotations,
               CircuitOp circuit, size_t &nlaNumber, Location loc,
               unsigned index) {
  // The input attribute must be a dictionary.
  DictionaryAttr dict = original.dyn_cast<DictionaryAttr>();
  if (!dict) {
    llvm::errs() << "OMField is not a dictionary, but should be: " << original
                 << "\n";
    return None;
  }

  auto *ctx = circuit.getContext();

  // Generate an arbitrary identifier to use for caching when using
  // `maybeStringToLocation`.
  StringAttr locatorFilenameCache = StringAttr::get(ctx, ".");
  FileLineColLoc fileLineColLocCache;

  // Convert location from a string to a location attribute.
  auto infoAttr = tryGetAs<StringAttr>(dict, root, "info", loc, omirAnnoClass);
  if (!infoAttr)
    return None;
  auto maybeLoc =
      maybeStringToLocation(infoAttr.getValue(), false, locatorFilenameCache,
                            fileLineColLocCache, ctx);
  mlir::LocationAttr infoLoc;
  if (maybeLoc.first)
    infoLoc = maybeLoc.second.getValue();
  else
    infoLoc = UnknownLoc::get(ctx);

  // Extract the name attribute.
  auto nameAttr = tryGetAs<StringAttr>(dict, root, "name", loc, omirAnnoClass);
  if (!nameAttr)
    return None;

  // The value attribute is unstructured and just copied over.
  auto valueAttr = tryGetAs<Attribute>(dict, root, "value", loc, omirAnnoClass);
  if (!valueAttr)
    return None;
  auto newValue =
      scatterOMIR(valueAttr, annotationID, newAnnotations, circuit, nlaNumber);
  if (!newValue)
    return None;

  NamedAttrList values;
  // We add the index if one was provided.  This can be used later to
  // reconstruct the order of the original array.
  values.append("index", IntegerAttr::get(IntegerType::get(ctx, 64), index));
  values.append("info", infoLoc);
  values.append("value", newValue.getValue());

  return {{nameAttr.getValue(), DictionaryAttr::getWithSorted(ctx, values)}};
}

/// Convert an Object Model Node to an optional dictionary, convert source
/// locator strings to location attributes, and scatter FIRRTL targets into the
/// circuit.  If this is an illegal Object Model Node, then return None.
///
/// An Object Model Node is expected to look like:
///
///   - "info": Source Locator String
///   - "id": String-encoded integer ('OMID' ':' Integer)
///   - "fields": Array<Object>
///
/// The "fields" member may be absent.  If so, then construct an empty array.
static Optional<DictionaryAttr>
scatterOMNode(Attribute original, const Attribute root, unsigned &annotationID,
              llvm::StringMap<llvm::SmallVector<Attribute>> &newAnnotations,
              CircuitOp circuit, size_t &nlaNumber, Location loc) {

  /// The input attribute must be a dictionary.
  DictionaryAttr dict = original.dyn_cast<DictionaryAttr>();
  if (!dict) {
    llvm::errs() << "OMNode is not a dictionary, but should be: " << original
                 << "\n";
    return None;
  }

  NamedAttrList omnode;
  auto *ctx = circuit.getContext();

  // Generate an arbitrary identifier to use for caching when using
  // `maybeStringToLocation`.
  StringAttr locatorFilenameCache = StringAttr::get(ctx, ".");
  FileLineColLoc fileLineColLocCache;

  // Convert the location from a string to a location attribute.
  auto infoAttr = tryGetAs<StringAttr>(dict, root, "info", loc, omirAnnoClass);
  if (!infoAttr)
    return None;
  auto maybeLoc =
      maybeStringToLocation(infoAttr.getValue(), false, locatorFilenameCache,
                            fileLineColLocCache, ctx);
  mlir::LocationAttr infoLoc;
  if (maybeLoc.first)
    infoLoc = maybeLoc.second.getValue();
  else
    infoLoc = UnknownLoc::get(ctx);

  // Extract the OMID.  Don't parse this, just leave it as a string.
  auto idAttr = tryGetAs<StringAttr>(dict, root, "id", loc, omirAnnoClass);
  if (!idAttr)
    return None;

  // Convert the fields from an ArrayAttr to a DictionaryAttr keyed by their
  // "name".  If no fields member exists, then just create an empty dictionary.
  // Note that this is safe to construct because all fields must have unique
  // "name" members relative to each other.
  auto maybeFields = dict.getAs<ArrayAttr>("fields");
  DictionaryAttr fields;
  if (!maybeFields)
    fields = DictionaryAttr::get(ctx);
  else {
    auto fieldAttr = maybeFields.getValue();
    NamedAttrList fieldAttrs;
    for (size_t i = 0, e = fieldAttr.size(); i != e; ++i) {
      auto field = fieldAttr[i];
      if (auto newField =
              scatterOMField(field, root, annotationID, newAnnotations, circuit,
                             nlaNumber, loc, i)) {
        fieldAttrs.append(newField.getValue().first,
                          newField.getValue().second);
        continue;
      }
      return None;
    }
    fields = DictionaryAttr::get(ctx, fieldAttrs);
  }

  omnode.append("fields", fields);
  omnode.append("id", idAttr);
  omnode.append("info", infoLoc);

  return DictionaryAttr::getWithSorted(ctx, omnode);
}

/// Main entry point to handle scattering of an OMIRAnnotation.  Return the
/// modified optional attribute on success and None on failure.  Any scattered
/// annotations will be added to the reference argument `newAnnotations`.
static Optional<Attribute> scatterOMIRAnnotation(
    DictionaryAttr dict, unsigned &annotationID,
    llvm::StringMap<llvm::SmallVector<Attribute>> &newAnnotations,
    CircuitOp circuit, size_t &nlaNumber, Location loc) {

  auto nodes = tryGetAs<ArrayAttr>(dict, dict, "nodes", loc, omirAnnoClass);
  if (!nodes)
    return None;

  SmallVector<Attribute> newNodes;
  for (auto node : nodes) {
    auto newNode = scatterOMNode(node, dict, annotationID, newAnnotations,
                                 circuit, nlaNumber, loc);
    if (!newNode)
      return None;
    newNodes.push_back(newNode.getValue());
  }

  auto *ctx = circuit.getContext();

  NamedAttrList newAnnotation;
  newAnnotation.append("class", StringAttr::get(ctx, omirAnnoClass));
  newAnnotation.append("nodes", ArrayAttr::get(ctx, newNodes));
  return DictionaryAttr::get(ctx, newAnnotation);
}

/// Convert known custom FIRRTL Annotations with compound targets to multiple
/// attributes that are attached to IR operations where they have semantic
/// meaning.  This rewrites the input \p annotationMap to convert non-specific
/// Annotations targeting "~" to those targeting something more specific if
/// possible.
bool circt::firrtl::scatterCustomAnnotations(
    llvm::StringMap<ArrayAttr> &annotationMap, CircuitOp circuit,
    unsigned &annotationID, Location loc, size_t &nlaNumber) {
  MLIRContext *context = circuit.getContext();

  // Exit if no anotations exist that target "~". Also ensure a spurious entry
  // is not created in the map.
  if (!annotationMap.count("~"))
    return true;
  // This adds an entry "~" to the map.
  auto nonSpecificAnnotations = annotationMap["~"];

  // Mutable store of new annotations produced.
  llvm::StringMap<llvm::SmallVector<Attribute>> newAnnotations;

  /// Return a new identifier that can be used to link scattered annotations
  /// together.  This mutates the by-reference parameter annotationID.
  auto newID = [&]() {
    return IntegerAttr::get(IntegerType::get(context, 64), annotationID++);
  };

  /// Add a don't touch annotation for a target.
  auto addDontTouch = [&](StringRef target,
                          Optional<ArrayAttr> subfields = {}) {
    NamedAttrList fields;
    fields.append(
        "class",
        StringAttr::get(context, "firrtl.transforms.DontTouchAnnotation"));
    if (subfields)
      fields.append("target", *subfields);
    newAnnotations[target].push_back(
        DictionaryAttr::getWithSorted(context, fields));
  };

  // Loop over all non-specific annotations that target "~".
  //
  //
  for (auto a : nonSpecificAnnotations) {
    auto dict = a.cast<DictionaryAttr>();
    StringAttr classAttr = dict.getAs<StringAttr>("class");
    // If the annotation doesn't have a "class" field, then we can't handle it.
    // Just copy it over.
    if (!classAttr) {
      newAnnotations["~"].push_back(a);
      continue;
    }

    // Get the "class" value and branch based on this.
    //
    // TODO: Determine a way to do this in an extensible way.  I.e., a user
    // should be able to register a handler for an annotation of a specific
    // class.
    StringRef clazz = classAttr.getValue();

    // Scatter signal driver annotations to the sources *and* the targets of the
    // drives.
    if (clazz == signalDriverAnnoClass) {
      auto id = newID();

      // Rework the circuit-level annotation to no longer include the
      // information we are scattering away anyway.
      NamedAttrList fields;
      auto annotationsAttr =
          tryGetAs<ArrayAttr>(dict, dict, "annotations", loc, clazz);
      auto circuitAttr =
          tryGetAs<StringAttr>(dict, dict, "circuit", loc, clazz);
      auto circuitPackageAttr =
          tryGetAs<StringAttr>(dict, dict, "circuitPackage", loc, clazz);
      if (!annotationsAttr || !circuitAttr || !circuitPackageAttr)
        return false;
      fields.append("class", classAttr);
      fields.append("id", id);
      fields.append("annotations", annotationsAttr);
      fields.append("circuit", circuitAttr);
      fields.append("circuitPackage", circuitPackageAttr);

      // A callback that will scatter every source and sink target pair to the
      // corresponding two ends of the connection.
      bool isSubCircuit = false;
      llvm::StringSet annotatedModules;
      auto handleTarget = [&](Attribute attr, unsigned i, bool isSource) {
        auto targetId = newID();
        DictionaryAttr targetDict = attr.dyn_cast<DictionaryAttr>();
        if (!targetDict) {
          mlir::emitError(loc, "SignalDriverAnnotation source and sink target "
                               "entries must be dictionaries")
                  .attachNote()
              << "annotation:" << dict << "\n";
          return false;
        }

        // Dig up the two sides of the link.
        auto path = (Twine(clazz) + "." + (isSource ? "source" : "sink") +
                     "Targets[" + Twine(i) + "]")
                        .str();
        auto remoteAttr =
            tryGetAs<StringAttr>(targetDict, dict, "_1", loc, path);
        auto localAttr =
            tryGetAs<StringAttr>(targetDict, dict, "_2", loc, path);
        if (!localAttr || !remoteAttr)
          return false;

        // Build the two annotations.
        for (auto pair : std::array{std::make_pair(localAttr, true),
                                    std::make_pair(remoteAttr, false)}) {
          auto canonTarget = oldCanonicalizeTarget(pair.first.getValue());
          if (!canonTarget)
            return false;

          // HACK: Ignore the side of the connection that targets the *other*
          // circuit. We do this by checking whether the canonicalized target
          // begins with `~CircuitName|`. If it doesn't, we skip.
          // TODO: Once we properly support multiple circuits, this can go and
          // the annotation can scatter properly.
          StringRef prefix(*canonTarget);
          if (!(prefix.consume_front("~") &&
                prefix.consume_front(circuit.name()) &&
                prefix.consume_front("|"))) {
            continue;
          }

          // If we get to this point, then we are processing the subcircuit.
          // Indicate that this requires JSON emission (whereas the main circuit
          // does not).
          if (pair.second)
            isSubCircuit = true;

          // Assemble the annotation on this side of the connection.
          NamedAttrList fields;
          fields.append("class", classAttr);
          fields.append("id", id);
          fields.append("targetId", targetId);
          fields.append("peer", pair.second ? remoteAttr : localAttr);
          fields.append("side", StringAttr::get(
                                    context, pair.second ? "local" : "remote"));
          fields.append("dir",
                        StringAttr::get(context, isSource ? "source" : "sink"));

          // Handle subfield and non-local targets.
          auto NLATargets = expandNonLocal(*canonTarget);
          auto leafTarget = splitAndAppendTarget(
              fields, std::get<0>(NLATargets.back()), context);
          FlatSymbolRefAttr nlaSym;
          if (NLATargets.size() > 1) {
            nlaSym = buildNLA(circuit, ++nlaNumber, NLATargets);
            fields.append("circt.nonlocal", nlaSym);
          }
          newAnnotations[leafTarget.first].push_back(
              DictionaryAttr::get(context, fields));

          // Add a don't touch annotation to whatever this annotation targets.
          addDontTouch(leafTarget.first, leafTarget.second);

          // Keep track of the enclosing module.
          annotatedModules.insert(
              (StringRef(std::get<0>(NLATargets.back())).split("|").first +
               "|" + std::get<1>(NLATargets.back()))
                  .str());

          // Annotate instances along the NLA path.
          for (int i = 0, e = NLATargets.size() - 1; i < e; ++i) {
            StringRef tgt = std::get<0>(NLATargets[i]);
            addDontTouch(tgt);
          }
        }

        return true;
      };

      // Handle the source and sink targets.
      auto sourcesAttr =
          tryGetAs<ArrayAttr>(dict, dict, "sourceTargets", loc, clazz);
      auto sinksAttr =
          tryGetAs<ArrayAttr>(dict, dict, "sinkTargets", loc, clazz);
      if (!sourcesAttr || !sinksAttr)
        return false;
      unsigned i = 0;
      for (auto attr : sourcesAttr)
        if (!handleTarget(attr, i++, true))
          return false;
      i = 0;
      for (auto attr : sinksAttr)
        if (!handleTarget(attr, i++, false))
          return false;

      // Add field indicating if we're the subcircuit or not.
      fields.append("isSubCircuit", BoolAttr::get(context, isSubCircuit));

      newAnnotations["~"].push_back(DictionaryAttr::get(context, fields));

      // Indicate which modules have embedded `SignalDriverAnnotation`s.
      for (auto &module : annotatedModules) {
        NamedAttrList fields;
        fields.append("class", classAttr);
        fields.append("id", id);
        newAnnotations[module.getKey()].push_back(
            DictionaryAttr::get(context, fields));
      }

      continue;
    }

    if (clazz == moduleReplacementAnnoClass) {
      auto id = newID();
      NamedAttrList fields;
      auto annotationsAttr =
          tryGetAs<ArrayAttr>(dict, dict, "annotations", loc, clazz);
      auto circuitAttr =
          tryGetAs<StringAttr>(dict, dict, "circuit", loc, clazz);
      auto circuitPackageAttr =
          tryGetAs<StringAttr>(dict, dict, "circuitPackage", loc, clazz);
      auto dontTouchesAttr =
          tryGetAs<ArrayAttr>(dict, dict, "dontTouches", loc, clazz);
      if (!annotationsAttr || !circuitAttr || !circuitPackageAttr ||
          !dontTouchesAttr)
        return false;
      fields.append("class", classAttr);
      fields.append("id", id);
      fields.append("annotations", annotationsAttr);
      fields.append("circuit", circuitAttr);
      fields.append("circuitPackage", circuitPackageAttr);
      newAnnotations["~"].push_back(DictionaryAttr::get(context, fields));

      // Add a don't touches for each target in "dontTouches" list
      for (auto dontTouch : dontTouchesAttr) {
        StringAttr targetString = dontTouch.dyn_cast<StringAttr>();
        if (!targetString) {
          mlir::emitError(
              loc,
              "ModuleReplacementAnnotation dontTouches entries must be strings")
                  .attachNote()
              << "annotation:" << dict << "\n";
          return false;
        }
        auto canonTarget = oldCanonicalizeTarget(targetString.getValue());
        if (!canonTarget)
          return false;
        auto nlaTargets = expandNonLocal(*canonTarget);
        auto leafTarget = splitAndAppendTarget(
            fields, std::get<0>(nlaTargets.back()), context);

        // Add a don't touch annotation to whatever this annotation targets.
        addDontTouch(leafTarget.first, leafTarget.second);
      }

      auto targets = tryGetAs<ArrayAttr>(dict, dict, "targets", loc, clazz);
      if (!targets)
        return false;
      for (auto targetAttr : targets) {
        NamedAttrList fields;
        fields.append("id", id);
        StringAttr targetString = targetAttr.dyn_cast<StringAttr>();
        if (!targetString) {
          mlir::emitError(
              loc,
              "ModuleReplacementAnnotation targets entries must be strings")
                  .attachNote()
              << "annotation:" << dict << "\n";
          return false;
        }
        auto canonTarget = oldCanonicalizeTarget(targetString.getValue());
        if (!canonTarget)
          return false;
        auto nlaTargets = expandNonLocal(*canonTarget);
        auto leafTarget = splitAndAppendTarget(
            fields, std::get<0>(nlaTargets.back()), context);
        FlatSymbolRefAttr nlaSym;
        if (nlaTargets.size() > 1) {
          nlaSym = buildNLA(circuit, ++nlaNumber, nlaTargets);
          fields.append("circt.nonlocal", nlaSym);
          addDontTouch(leafTarget.first);
        }
        newAnnotations[leafTarget.first].push_back(
            DictionaryAttr::get(context, fields));

        // Annotate instances along the NLA path.
        for (int i = 0, e = nlaTargets.size() - 1; i < e; ++i) {
          addDontTouch(std::get<0>(nlaTargets[i]));
        }
      }
    }

    // Scatter trackers out from OMIR JSON.
    if (clazz == omirAnnoClass) {
      auto newAnno = scatterOMIRAnnotation(dict, annotationID, newAnnotations,
                                           circuit, nlaNumber, loc);
      if (!newAnno)
        return false;
      newAnnotations["~"].push_back(newAnno.getValue());
      continue;
    }

    // Just copy over any annotation we don't understand.
    newAnnotations["~"].push_back(a);
  }

  // Delete all the old CircuitTarget annotations.
  annotationMap.erase("~");

  // Convert the mutable Annotation map to a SmallVector<ArrayAttr>.
  for (auto a : newAnnotations.keys()) {
    // If multiple annotations on a single object, then append it.
    if (annotationMap.count(a))
      for (auto attr : annotationMap[a])
        newAnnotations[a].push_back(attr);

    annotationMap[a] = ArrayAttr::get(context, newAnnotations[a]);
  }

  return true;
}

/// Deserialize a JSON value into FIRRTL Annotations.  Annotations are
/// represented as a Target-keyed arrays of attributes.  The input JSON value is
/// checked, at runtime, to be an array of objects.  Returns true if successful,
/// false if unsuccessful.
bool circt::firrtl::fromJSONRaw(json::Value &value, StringRef circuitTarget,
                                SmallVectorImpl<Attribute> &attrs,
                                json::Path path, MLIRContext *context) {

  // The JSON value must be an array of objects.  Anything else is reported as
  // invalid.
  auto array = value.getAsArray();
  if (!array) {
    path.report(
        "Expected annotations to be an array, but found something else.");
    return false;
  }

  // Build an array of annotations.
  for (size_t i = 0, e = (*array).size(); i != e; ++i) {
    auto object = (*array)[i].getAsObject();
    auto p = path.index(i);
    if (!object) {
      p.report("Expected annotations to be an array of objects, but found an "
               "array of something else.");
      return false;
    }

    // Build up the Attribute to represent the Annotation
    NamedAttrList metadata;

    for (auto field : *object) {
      if (auto value = convertJSONToAttribute(context, field.second, p)) {
        metadata.append(field.first, value);
        continue;
      }
      return false;
    }

    attrs.push_back(DictionaryAttr::get(context, metadata));
  }

  return true;
}
