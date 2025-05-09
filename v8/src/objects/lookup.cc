// Copyright 2014 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/objects/lookup.h"

#include "src/common/globals.h"
#include "src/deoptimizer/deoptimizer.h"
#include "src/execution/isolate-inl.h"
#include "src/execution/protectors-inl.h"
#include "src/init/bootstrapper.h"
#include "src/logging/counters.h"
#include "src/objects/arguments-inl.h"
#include "src/objects/elements.h"
#include "src/objects/field-type.h"
#include "src/objects/hash-table-inl.h"
#include "src/objects/heap-number-inl.h"
#include "src/objects/js-shared-array-inl.h"
#include "src/objects/js-struct-inl.h"
#include "src/objects/map-updater.h"
#include "src/objects/ordered-hash-table.h"
#include "src/objects/property-details.h"
#include "src/objects/struct-inl.h"

namespace v8 {
namespace internal {

PropertyKey::PropertyKey(Isolate* isolate, Handle<Object> key, bool* success) {
  if (key->ToIntegerIndex(&index_)) {
    *success = true;
    return;
  }
  *success = Object::ToName(isolate, key).ToHandle(&name_);
  if (!*success) {
    DCHECK(isolate->has_pending_exception());
    index_ = LookupIterator::kInvalidIndex;
    return;
  }
  if (!name_->AsIntegerIndex(&index_)) {
    // {AsIntegerIndex} may modify {index_} before deciding to fail.
    index_ = LookupIterator::kInvalidIndex;
  }
}

template <bool is_element>
void LookupIterator::Start() {
  // GetRoot might allocate if lookup_start_object_ is a string.
  MaybeHandle<JSReceiver> maybe_holder =
      GetRoot(isolate_, lookup_start_object_, index_, configuration_);
  if (!maybe_holder.ToHandle(&holder_)) {
    // This is an attempt to perform an own property lookup on a non-JSReceiver
    // that doesn't have any properties.
    DCHECK(!lookup_start_object_->IsJSReceiver());
    DCHECK(!check_prototype_chain());
    has_property_ = false;
    state_ = NOT_FOUND;
    return;
  }

  {
    DisallowGarbageCollection no_gc;

    has_property_ = false;
    state_ = NOT_FOUND;

    JSReceiver holder = *holder_;
    Map map = holder.map(isolate_);

    state_ = LookupInHolder<is_element>(map, holder);
    if (IsFound()) return;

    NextInternal<is_element>(map, holder);
  }
}

template void LookupIterator::Start<true>();
template void LookupIterator::Start<false>();

void LookupIterator::Next() {
  DCHECK_NE(JSPROXY, state_);
  DCHECK_NE(TRANSITION, state_);
  DisallowGarbageCollection no_gc;
  has_property_ = false;

  JSReceiver holder = *holder_;
  Map map = holder.map(isolate_);

  if (map.IsSpecialReceiverMap()) {
    state_ = IsElement() ? LookupInSpecialHolder<true>(map, holder)
                         : LookupInSpecialHolder<false>(map, holder);
    if (IsFound()) return;
  }

  IsElement() ? NextInternal<true>(map, holder)
              : NextInternal<false>(map, holder);
}

template <bool is_element>
void LookupIterator::NextInternal(Map map, JSReceiver holder) {
  do {
    JSReceiver maybe_holder = NextHolder(map);
    if (maybe_holder.is_null()) {
      if (interceptor_state_ == InterceptorState::kSkipNonMasking) {
        RestartLookupForNonMaskingInterceptors<is_element>();
        return;
      }
      state_ = NOT_FOUND;
      if (holder != *holder_) holder_ = handle(holder, isolate_);
      return;
    }
    holder = maybe_holder;
    map = holder.map(isolate_);
    state_ = LookupInHolder<is_element>(map, holder);
  } while (!IsFound());

  holder_ = handle(holder, isolate_);
}

template <bool is_element>
void LookupIterator::RestartInternal(InterceptorState interceptor_state) {
  interceptor_state_ = interceptor_state;
  property_details_ = PropertyDetails::Empty();
  number_ = InternalIndex::NotFound();
  Start<is_element>();
}

template void LookupIterator::RestartInternal<true>(InterceptorState);
template void LookupIterator::RestartInternal<false>(InterceptorState);

// static
MaybeHandle<JSReceiver> LookupIterator::GetRootForNonJSReceiver(
    Isolate* isolate, Handle<Object> lookup_start_object, size_t index,
    Configuration configuration) {
  // Strings are the only non-JSReceiver objects with properties (only elements
  // and 'length') directly on the wrapper. Hence we can skip generating
  // the wrapper for all other cases.
  bool own_property_lookup = (configuration & kPrototypeChain) == 0;
  if (lookup_start_object->IsString(isolate)) {
    if (own_property_lookup ||
        index <
            static_cast<size_t>(String::cast(*lookup_start_object).length())) {
      // TODO(verwaest): Speed this up. Perhaps use a cached wrapper on the
      // native context, ensuring that we don't leak it into JS?
      Handle<JSFunction> constructor = isolate->string_function();
      Handle<JSObject> result = isolate->factory()->NewJSObject(constructor);
      Handle<JSPrimitiveWrapper>::cast(result)->set_value(*lookup_start_object);
      return result;
    }
  } else if (own_property_lookup) {
    // Signal that the lookup will not find anything.
    return {};
  }
  Handle<HeapObject> root(
      lookup_start_object->GetPrototypeChainRootMap(isolate).prototype(isolate),
      isolate);
  if (root->IsNull(isolate)) {
    isolate->PushStackTraceAndDie(
        reinterpret_cast<void*>(lookup_start_object->ptr()));
  }
  return Handle<JSReceiver>::cast(root);
}

Handle<Map> LookupIterator::GetReceiverMap() const {
  if (receiver_->IsNumber(isolate_)) return factory()->heap_number_map();
  return handle(Handle<HeapObject>::cast(receiver_)->map(isolate_), isolate_);
}

bool LookupIterator::HasAccess() const {
  // TRANSITION is true when being called from DefineNamedOwnIC.
  DCHECK(state_ == ACCESS_CHECK || state_ == TRANSITION);
  return isolate_->MayAccess(handle(isolate_->context(), isolate_),
                             GetHolder<JSObject>());
}

template <bool is_element>
void LookupIterator::ReloadPropertyInformation() {
  state_ = BEFORE_PROPERTY;
  interceptor_state_ = InterceptorState::kUninitialized;
  state_ = LookupInHolder<is_element>(holder_->map(isolate_), *holder_);
  DCHECK(IsFound() || !holder_->HasFastProperties(isolate_));
}

// static
void LookupIterator::InternalUpdateProtector(Isolate* isolate,
                                             Handle<Object> receiver_generic,
                                             Handle<Name> name) {
  if (isolate->bootstrapper()->IsActive()) return;
  if (!receiver_generic->IsHeapObject()) return;
  Handle<HeapObject> receiver = Handle<HeapObject>::cast(receiver_generic);

  ReadOnlyRoots roots(isolate);
  if (*name == roots.constructor_string()) {
    // Setting the constructor property could change an instance's @@species
    if (receiver->IsJSArray(isolate)) {
      if (!Protectors::IsArraySpeciesLookupChainIntact(isolate)) return;
      isolate->CountUsage(
          v8::Isolate::UseCounterFeature::kArrayInstanceConstructorModified);
      Protectors::InvalidateArraySpeciesLookupChain(isolate);
      return;
    } else if (receiver->IsJSPromise(isolate)) {
      if (!Protectors::IsPromiseSpeciesLookupChainIntact(isolate)) return;
      Protectors::InvalidatePromiseSpeciesLookupChain(isolate);
      return;
    } else if (receiver->IsJSRegExp(isolate)) {
      if (!Protectors::IsRegExpSpeciesLookupChainIntact(isolate)) return;
      Protectors::InvalidateRegExpSpeciesLookupChain(isolate);
      return;
    } else if (receiver->IsJSTypedArray(isolate)) {
      if (!Protectors::IsTypedArraySpeciesLookupChainIntact(isolate)) return;
      Protectors::InvalidateTypedArraySpeciesLookupChain(isolate);
      return;
    }
    if (receiver->map(isolate).is_prototype_map()) {
      DisallowGarbageCollection no_gc;
      // Setting the constructor of any prototype with the @@species protector
      // (of any realm) also needs to invalidate the protector.
      if (isolate->IsInAnyContext(*receiver,
                                  Context::INITIAL_ARRAY_PROTOTYPE_INDEX)) {
        if (!Protectors::IsArraySpeciesLookupChainIntact(isolate)) return;
        isolate->CountUsage(
            v8::Isolate::UseCounterFeature::kArrayPrototypeConstructorModified);
        Protectors::InvalidateArraySpeciesLookupChain(isolate);
      } else if (receiver->IsJSPromisePrototype()) {
        if (!Protectors::IsPromiseSpeciesLookupChainIntact(isolate)) return;
        Protectors::InvalidatePromiseSpeciesLookupChain(isolate);
      } else if (receiver->IsJSRegExpPrototype()) {
        if (!Protectors::IsRegExpSpeciesLookupChainIntact(isolate)) return;
        Protectors::InvalidateRegExpSpeciesLookupChain(isolate);
      } else if (receiver->IsJSTypedArrayPrototype()) {
        if (!Protectors::IsTypedArraySpeciesLookupChainIntact(isolate)) return;
        Protectors::InvalidateTypedArraySpeciesLookupChain(isolate);
      }
    }
  } else if (*name == roots.next_string()) {
    if (receiver->IsJSArrayIterator() ||
        receiver->IsJSArrayIteratorPrototype()) {
      // Setting the next property of %ArrayIteratorPrototype% also needs to
      // invalidate the array iterator protector.
      if (!Protectors::IsArrayIteratorLookupChainIntact(isolate)) return;
      Protectors::InvalidateArrayIteratorLookupChain(isolate);
    } else if (receiver->IsJSMapIterator() ||
               receiver->IsJSMapIteratorPrototype()) {
      if (!Protectors::IsMapIteratorLookupChainIntact(isolate)) return;
      Protectors::InvalidateMapIteratorLookupChain(isolate);
    } else if (receiver->IsJSSetIterator() ||
               receiver->IsJSSetIteratorPrototype()) {
      if (!Protectors::IsSetIteratorLookupChainIntact(isolate)) return;
      Protectors::InvalidateSetIteratorLookupChain(isolate);
    } else if (receiver->IsJSStringIterator() ||
               receiver->IsJSStringIteratorPrototype()) {
      // Setting the next property of %StringIteratorPrototype% invalidates the
      // string iterator protector.
      if (!Protectors::IsStringIteratorLookupChainIntact(isolate)) return;
      Protectors::InvalidateStringIteratorLookupChain(isolate);
    }
  } else if (*name == roots.species_symbol()) {
    // Setting the Symbol.species property of any Array, Promise or TypedArray
    // constructor invalidates the @@species protector
    if (receiver->IsJSArrayConstructor()) {
      if (!Protectors::IsArraySpeciesLookupChainIntact(isolate)) return;
      isolate->CountUsage(
          v8::Isolate::UseCounterFeature::kArraySpeciesModified);
      Protectors::InvalidateArraySpeciesLookupChain(isolate);
    } else if (receiver->IsJSPromiseConstructor()) {
      if (!Protectors::IsPromiseSpeciesLookupChainIntact(isolate)) return;
      Protectors::InvalidatePromiseSpeciesLookupChain(isolate);
    } else if (receiver->IsJSRegExpConstructor()) {
      if (!Protectors::IsRegExpSpeciesLookupChainIntact(isolate)) return;
      Protectors::InvalidateRegExpSpeciesLookupChain(isolate);
    } else if (receiver->IsTypedArrayConstructor()) {
      if (!Protectors::IsTypedArraySpeciesLookupChainIntact(isolate)) return;
      Protectors::InvalidateTypedArraySpeciesLookupChain(isolate);
    }
  } else if (*name == roots.is_concat_spreadable_symbol()) {
    if (!Protectors::IsIsConcatSpreadableLookupChainIntact(isolate)) return;
    Protectors::InvalidateIsConcatSpreadableLookupChain(isolate);
  } else if (*name == roots.iterator_symbol()) {
    if (receiver->IsJSArray(isolate)) {
      if (!Protectors::IsArrayIteratorLookupChainIntact(isolate)) return;
      Protectors::InvalidateArrayIteratorLookupChain(isolate);
    } else if (receiver->IsJSSet(isolate) || receiver->IsJSSetIterator() ||
               receiver->IsJSSetIteratorPrototype() ||
               receiver->IsJSSetPrototype()) {
      if (Protectors::IsSetIteratorLookupChainIntact(isolate)) {
        Protectors::InvalidateSetIteratorLookupChain(isolate);
      }
    } else if (receiver->IsJSMapIterator() ||
               receiver->IsJSMapIteratorPrototype()) {
      if (Protectors::IsMapIteratorLookupChainIntact(isolate)) {
        Protectors::InvalidateMapIteratorLookupChain(isolate);
      }
    } else if (receiver->IsJSIteratorPrototype()) {
      if (Protectors::IsMapIteratorLookupChainIntact(isolate)) {
        Protectors::InvalidateMapIteratorLookupChain(isolate);
      }
      if (Protectors::IsSetIteratorLookupChainIntact(isolate)) {
        Protectors::InvalidateSetIteratorLookupChain(isolate);
      }
    } else if (isolate->IsInAnyContext(
                   *receiver, Context::INITIAL_STRING_PROTOTYPE_INDEX)) {
      // Setting the Symbol.iterator property of String.prototype invalidates
      // the string iterator protector. Symbol.iterator can also be set on a
      // String wrapper, but not on a primitive string. We only support
      // protector for primitive strings.
      if (!Protectors::IsStringIteratorLookupChainIntact(isolate)) return;
      Protectors::InvalidateStringIteratorLookupChain(isolate);
    }
  } else if (*name == roots.resolve_string()) {
    if (!Protectors::IsPromiseResolveLookupChainIntact(isolate)) return;
    // Setting the "resolve" property on any %Promise% intrinsic object
    // invalidates the Promise.resolve protector.
    if (receiver->IsJSPromiseConstructor()) {
      Protectors::InvalidatePromiseResolveLookupChain(isolate);
    }
  } else if (*name == roots.then_string()) {
    if (!Protectors::IsPromiseThenLookupChainIntact(isolate)) return;
    // Setting the "then" property on any JSPromise instance or on the
    // initial %PromisePrototype% invalidates the Promise#then protector.
    // Also setting the "then" property on the initial %ObjectPrototype%
    // invalidates the Promise#then protector, since we use this protector
    // to guard the fast-path in AsyncGeneratorResolve, where we can skip
    // the ResolvePromise step and go directly to FulfillPromise if we
    // know that the Object.prototype doesn't contain a "then" method.
    if (receiver->IsJSPromise(isolate) || receiver->IsJSObjectPrototype() ||
        receiver->IsJSPromisePrototype()) {
      Protectors::InvalidatePromiseThenLookupChain(isolate);
    }
  } else if (*name == roots.replace_symbol()) {
    if (!Protectors::IsNumberStringPrototypeNoReplaceIntact(isolate)) return;
    // We need to protect the prototype chains of `Number.prototype` and
    // `String.prototype`: that `Symbol.replace` is not added as a property on
    // any object on these prototype chains.
    // We detect `Number.prototype` and `String.prototype` by checking for a
    // prototype that is a JSPrimitiveWrapper. This is a safe approximation.
    // Using JSPrimitiveWrapper as prototype should be sufficiently rare.
    if (receiver->map().is_prototype_map() &&
        (receiver->IsJSPrimitiveWrapper() || receiver->IsJSObjectPrototype())) {
      Protectors::InvalidateNumberStringPrototypeNoReplace(isolate);
    }
  }
}

void LookupIterator::PrepareForDataProperty(Handle<Object> value) {
  DCHECK(state_ == DATA || state_ == ACCESSOR);
  DCHECK(HolderIsReceiverOrHiddenPrototype());
  DCHECK(!receiver_->IsWasmObject(isolate_));

  Handle<JSReceiver> holder = GetHolder<JSReceiver>();
  // We are not interested in tracking constness of a JSProxy's direct
  // properties.
  DCHECK_IMPLIES(holder->IsJSProxy(isolate_), name()->IsPrivate(isolate_));
  if (holder->IsJSProxy(isolate_)) return;

  if (IsElement(*holder)) {
    Handle<JSObject> holder_obj = Handle<JSObject>::cast(holder);
    ElementsKind kind = holder_obj->GetElementsKind(isolate_);
    ElementsKind to = value->OptimalElementsKind(isolate_);
    if (IsHoleyElementsKind(kind)) to = GetHoleyElementsKind(to);
    to = GetMoreGeneralElementsKind(kind, to);

    if (kind != to) {
      JSObject::TransitionElementsKind(holder_obj, to);
    }

    // Copy the backing store if it is copy-on-write.
    if (IsSmiOrObjectElementsKind(to) || IsSealedElementsKind(to) ||
        IsNonextensibleElementsKind(to)) {
      JSObject::EnsureWritableFastElements(holder_obj);
    }
    return;
  }

  if (holder->IsJSGlobalObject(isolate_)) {
    Handle<GlobalDictionary> dictionary(
        JSGlobalObject::cast(*holder).global_dictionary(isolate_, kAcquireLoad),
        isolate());
    Handle<PropertyCell> cell(dictionary->CellAt(isolate_, dictionary_entry()),
                              isolate());
    property_details_ = cell->property_details();
    PropertyCell::PrepareForAndSetValue(
        isolate(), dictionary, dictionary_entry(), value, property_details_);
    return;
  }

  PropertyConstness new_constness = PropertyConstness::kConst;
  if (constness() == PropertyConstness::kConst) {
    DCHECK_EQ(PropertyKind::kData, property_details_.kind());
    // Check that current value matches new value otherwise we should make
    // the property mutable.
    if (holder->HasFastProperties(isolate_)) {
      if (!CanStayConst(*value)) new_constness = PropertyConstness::kMutable;
    } else if (V8_DICT_PROPERTY_CONST_TRACKING_BOOL) {
      if (!DictCanStayConst(*value)) {
        property_details_ =
            property_details_.CopyWithConstness(PropertyConstness::kMutable);

        // We won't reach the map updating code after Map::Update below, because
        // that's only for the case that the existing map is a fast mode map.
        // Therefore, we need to perform the necessary updates to the property
        // details and the prototype validity cell directly.
        if constexpr (V8_ENABLE_SWISS_NAME_DICTIONARY_BOOL) {
          SwissNameDictionary dict = holder->property_dictionary_swiss();
          dict.DetailsAtPut(dictionary_entry(), property_details_);
        } else {
          NameDictionary dict = holder->property_dictionary();
          dict.DetailsAtPut(dictionary_entry(), property_details_);
        }

        Map old_map = holder->map(isolate_);
        if (old_map.is_prototype_map()) {
          JSObject::InvalidatePrototypeChains(old_map);
        }
      }
      return;
    }
  }

  if (!holder->HasFastProperties(isolate_)) return;

  Handle<JSObject> holder_obj = Handle<JSObject>::cast(holder);
  Handle<Map> old_map(holder->map(isolate_), isolate_);

  Handle<Map> new_map = Map::Update(isolate_, old_map);
  if (!new_map->is_dictionary_map()) {  // fast -> fast
    new_map = Map::PrepareForDataProperty(
        isolate(), new_map, descriptor_number(), new_constness, value);

    if (old_map.is_identical_to(new_map)) {
      // Update the property details if the representation was None.
      if (constness() != new_constness || representation().IsNone()) {
        property_details_ = new_map->instance_descriptors(isolate_).GetDetails(
            descriptor_number());
      }
      return;
    }
  }
  // We should only get here if the new_map is different from the old map,
  // otherwise we would have falled through to the is_identical_to check above.
  DCHECK_NE(*old_map, *new_map);

  JSObject::MigrateToMap(isolate_, holder_obj, new_map);
  ReloadPropertyInformation<false>();

  // If we transitioned from fast to slow and the property changed from kConst
  // to kMutable, then this change in the constness is indicated by neither the
  // old or the new map. We need to update the constness ourselves.
  DCHECK(!old_map->is_dictionary_map());
  if (V8_DICT_PROPERTY_CONST_TRACKING_BOOL && new_map->is_dictionary_map() &&
      new_constness == PropertyConstness::kMutable) {  // fast -> slow
    property_details_ =
        property_details_.CopyWithConstness(PropertyConstness::kMutable);

    if constexpr (V8_ENABLE_SWISS_NAME_DICTIONARY_BOOL) {
      SwissNameDictionary dict = holder_obj->property_dictionary_swiss();
      dict.DetailsAtPut(dictionary_entry(), property_details_);
    } else {
      NameDictionary dict = holder_obj->property_dictionary();
      dict.DetailsAtPut(dictionary_entry(), property_details_);
    }

    DCHECK_IMPLIES(new_map->is_prototype_map(),
                   !new_map->IsPrototypeValidityCellValid());
  }
}

void LookupIterator::ReconfigureDataProperty(Handle<Object> value,
                                             PropertyAttributes attributes) {
  DCHECK(state_ == DATA || state_ == ACCESSOR);
  DCHECK(HolderIsReceiverOrHiddenPrototype());

  Handle<JSReceiver> holder = GetHolder<JSReceiver>();
  if (V8_UNLIKELY(holder->IsWasmObject())) UNREACHABLE();

  // Property details can never change for private properties.
  if (holder->IsJSProxy(isolate_)) {
    DCHECK(name()->IsPrivate(isolate_));
    return;
  }

  Handle<JSObject> holder_obj = Handle<JSObject>::cast(holder);
  if (IsElement(*holder)) {
    DCHECK(!holder_obj->HasTypedArrayOrRabGsabTypedArrayElements(isolate_));
    DCHECK(attributes != NONE || !holder_obj->HasFastElements(isolate_));
    Handle<FixedArrayBase> elements(holder_obj->elements(isolate_), isolate());
    holder_obj->GetElementsAccessor(isolate_)->Reconfigure(
        holder_obj, elements, number_, value, attributes);
    ReloadPropertyInformation<true>();
  } else if (holder_obj->HasFastProperties(isolate_)) {
    Handle<Map> old_map(holder_obj->map(isolate_), isolate_);
    // Force mutable to avoid changing constant value by reconfiguring
    // kData -> kAccessor -> kData.
    Handle<Map> new_map = MapUpdater::ReconfigureExistingProperty(
        isolate_, old_map, descriptor_number(), i::PropertyKind::kData,
        attributes, PropertyConstness::kMutable);
    if (!new_map->is_dictionary_map()) {
      // Make sure that the data property has a compatible representation.
      // TODO(leszeks): Do this as part of ReconfigureExistingProperty.
      new_map =
          Map::PrepareForDataProperty(isolate(), new_map, descriptor_number(),
                                      PropertyConstness::kMutable, value);
    }
    JSObject::MigrateToMap(isolate_, holder_obj, new_map);
    ReloadPropertyInformation<false>();
  }

  if (!IsElement(*holder) && !holder_obj->HasFastProperties(isolate_)) {
    if (holder_obj->map(isolate_).is_prototype_map() &&
        (((property_details_.attributes() & READ_ONLY) == 0 &&
          (attributes & READ_ONLY) != 0) ||
         (property_details_.attributes() & DONT_ENUM) !=
             (attributes & DONT_ENUM))) {
      // Invalidate prototype validity cell when a property is reconfigured
      // from writable to read-only as this may invalidate transitioning store
      // IC handlers.
      // Invalidate prototype validity cell when a property changes
      // enumerability to clear the prototype chain enum cache.
      JSObject::InvalidatePrototypeChains(holder->map(isolate_));
    }
    if (holder_obj->IsJSGlobalObject(isolate_)) {
      PropertyDetails details(PropertyKind::kData, attributes,
                              PropertyCellType::kMutable);
      Handle<GlobalDictionary> dictionary(
          JSGlobalObject::cast(*holder_obj)
              .global_dictionary(isolate_, kAcquireLoad),
          isolate());

      Handle<PropertyCell> cell = PropertyCell::PrepareForAndSetValue(
          isolate(), dictionary, dictionary_entry(), value, details);
      property_details_ = cell->property_details();
      DCHECK_EQ(cell->value(), *value);
    } else {
      PropertyDetails details(PropertyKind::kData, attributes,
                              PropertyConstness::kMutable);
      if constexpr (V8_ENABLE_SWISS_NAME_DICTIONARY_BOOL) {
        Handle<SwissNameDictionary> dictionary(
            holder_obj->property_dictionary_swiss(isolate_), isolate());
        dictionary->ValueAtPut(dictionary_entry(), *value);
        dictionary->DetailsAtPut(dictionary_entry(), details);
        DCHECK_EQ(details.AsSmi(),
                  dictionary->DetailsAt(dictionary_entry()).AsSmi());
        property_details_ = details;
      } else {
        Handle<NameDictionary> dictionary(
            holder_obj->property_dictionary(isolate_), isolate());
        PropertyDetails original_details =
            dictionary->DetailsAt(dictionary_entry());
        int enumeration_index = original_details.dictionary_index();
        DCHECK_GT(enumeration_index, 0);
        details = details.set_index(enumeration_index);
        dictionary->SetEntry(dictionary_entry(), *name(), *value, details);
        property_details_ = details;
      }
    }
    state_ = DATA;
  }

  WriteDataValue(value, true);

#if VERIFY_HEAP
  if (v8_flags.verify_heap) {
    holder->HeapObjectVerify(isolate());
  }
#endif
}

// Can only be called when the receiver is a JSObject, or when the name is a
// private field, otherwise JSProxy has to be handled via a trap.
// Adding properties to primitive values is not observable.
void LookupIterator::PrepareTransitionToDataProperty(
    Handle<JSReceiver> receiver, Handle<Object> value,
    PropertyAttributes attributes, StoreOrigin store_origin) {
  DCHECK_IMPLIES(receiver->IsJSProxy(isolate_), name()->IsPrivate(isolate_));
  DCHECK_IMPLIES(!receiver.is_identical_to(GetStoreTarget<JSReceiver>()),
                 name()->IsPrivateName());
  DCHECK(!receiver->IsAlwaysSharedSpaceJSObject());
  if (state_ == TRANSITION) return;

  if (!IsElement() && name()->IsPrivate(isolate_)) {
    attributes = static_cast<PropertyAttributes>(attributes | DONT_ENUM);
  }

  DCHECK(state_ != LookupIterator::ACCESSOR ||
         (GetAccessors()->IsAccessorInfo(isolate_) &&
          AccessorInfo::cast(*GetAccessors()).is_special_data_property()));
  DCHECK_NE(INTEGER_INDEXED_EXOTIC, state_);
  DCHECK(state_ == NOT_FOUND || !HolderIsReceiverOrHiddenPrototype());

  Handle<Map> map(receiver->map(isolate_), isolate_);

  // Dictionary maps can always have additional data properties.
  if (map->is_dictionary_map()) {
    state_ = TRANSITION;
    if (map->IsJSGlobalObjectMap()) {
      DCHECK(!value->IsTheHole(isolate_));
      // Don't set enumeration index (it will be set during value store).
      property_details_ =
          PropertyDetails(PropertyKind::kData, attributes,
                          PropertyCell::InitialType(isolate_, *value));
      transition_ = isolate_->factory()->NewPropertyCell(
          name(), property_details_, value);
      has_property_ = true;
    } else {
      // Don't set enumeration index (it will be set during value store).
      property_details_ =
          PropertyDetails(PropertyKind::kData, attributes,
                          PropertyDetails::kConstIfDictConstnessTracking);
      transition_ = map;
    }
    return;
  }

  Handle<Map> transition =
      Map::TransitionToDataProperty(isolate_, map, name_, value, attributes,
                                    PropertyConstness::kConst, store_origin);
  state_ = TRANSITION;
  transition_ = transition;

  if (transition->is_dictionary_map()) {
    DCHECK(!transition->IsJSGlobalObjectMap());
    // Don't set enumeration index (it will be set during value store).
    property_details_ =
        PropertyDetails(PropertyKind::kData, attributes,
                        PropertyDetails::kConstIfDictConstnessTracking);
  } else {
    property_details_ = transition->GetLastDescriptorDetails(isolate_);
    has_property_ = true;
  }
}

void LookupIterator::ApplyTransitionToDataProperty(
    Handle<JSReceiver> receiver) {
  DCHECK_EQ(TRANSITION, state_);

  DCHECK_IMPLIES(!receiver.is_identical_to(GetStoreTarget<JSReceiver>()),
                 name()->IsPrivateName());
  holder_ = receiver;
  if (receiver->IsJSGlobalObject(isolate_)) {
    JSObject::InvalidatePrototypeChains(receiver->map(isolate_));

    // Install a property cell.
    Handle<JSGlobalObject> global = Handle<JSGlobalObject>::cast(receiver);
    DCHECK(!global->HasFastProperties());
    Handle<GlobalDictionary> dictionary(
        global->global_dictionary(isolate_, kAcquireLoad), isolate_);

    dictionary =
        GlobalDictionary::Add(isolate_, dictionary, name(), transition_cell(),
                              property_details_, &number_);
    global->set_global_dictionary(*dictionary, kReleaseStore);

    // Reload details containing proper enumeration index value.
    property_details_ = transition_cell()->property_details();
    has_property_ = true;
    state_ = DATA;
    return;
  }
  Handle<Map> transition = transition_map();
  bool simple_transition =
      transition->GetBackPointer(isolate_) == receiver->map(isolate_);

  if (configuration_ == DEFAULT && !transition->is_dictionary_map() &&
      !transition->IsPrototypeValidityCellValid()) {
    // Only LookupIterator instances with DEFAULT (full prototype chain)
    // configuration can produce valid transition handler maps.
    Handle<Object> validity_cell =
        Map::GetOrCreatePrototypeChainValidityCell(transition, isolate());
    transition->set_prototype_validity_cell(*validity_cell, kRelaxedStore);
  }

  if (!receiver->IsJSProxy(isolate_)) {
    JSObject::MigrateToMap(isolate_, Handle<JSObject>::cast(receiver),
                           transition);
  }

  if (simple_transition) {
    number_ = transition->LastAdded();
    property_details_ = transition->GetLastDescriptorDetails(isolate_);
    state_ = DATA;
  } else if (receiver->map(isolate_).is_dictionary_map()) {
    if (receiver->map(isolate_).is_prototype_map() &&
        receiver->IsJSObject(isolate_)) {
      JSObject::InvalidatePrototypeChains(receiver->map(isolate_));
    }
    if constexpr (V8_ENABLE_SWISS_NAME_DICTIONARY_BOOL) {
      Handle<SwissNameDictionary> dictionary(
          receiver->property_dictionary_swiss(isolate_), isolate_);

      dictionary =
          SwissNameDictionary::Add(isolate(), dictionary, name(),
                                   isolate_->factory()->uninitialized_value(),
                                   property_details_, &number_);
      receiver->SetProperties(*dictionary);
    } else {
      Handle<NameDictionary> dictionary(receiver->property_dictionary(isolate_),
                                        isolate_);

      dictionary =
          NameDictionary::Add(isolate(), dictionary, name(),
                              isolate_->factory()->uninitialized_value(),
                              property_details_, &number_);
      receiver->SetProperties(*dictionary);
      // TODO(pthier): Add flags to swiss dictionaries.
      if (name()->IsInterestingSymbol()) {
        dictionary->set_may_have_interesting_symbols(true);
      }
      // Reload details containing proper enumeration index value.
      property_details_ = dictionary->DetailsAt(number_);
    }
    has_property_ = true;
    state_ = DATA;

  } else {
    ReloadPropertyInformation<false>();
  }
}

void LookupIterator::Delete() {
  Handle<JSReceiver> holder = Handle<JSReceiver>::cast(holder_);
  if (IsElement(*holder)) {
    Handle<JSObject> object = Handle<JSObject>::cast(holder);
    ElementsAccessor* accessor = object->GetElementsAccessor(isolate_);
    accessor->Delete(object, number_);
  } else {
    DCHECK(!name()->IsPrivateName(isolate_));
    bool is_prototype_map = holder->map(isolate_).is_prototype_map();
    RCS_SCOPE(isolate_,
              is_prototype_map
                  ? RuntimeCallCounterId::kPrototypeObject_DeleteProperty
                  : RuntimeCallCounterId::kObject_DeleteProperty);

    PropertyNormalizationMode mode =
        is_prototype_map ? KEEP_INOBJECT_PROPERTIES : CLEAR_INOBJECT_PROPERTIES;

    if (holder->HasFastProperties(isolate_)) {
      JSObject::NormalizeProperties(isolate_, Handle<JSObject>::cast(holder),
                                    mode, 0, "DeletingProperty");
      ReloadPropertyInformation<false>();
    }
    JSReceiver::DeleteNormalizedProperty(holder, dictionary_entry());
    if (holder->IsJSObject(isolate_)) {
      JSObject::ReoptimizeIfPrototype(Handle<JSObject>::cast(holder));
    }
  }
  state_ = NOT_FOUND;
}

void LookupIterator::TransitionToAccessorProperty(
    Handle<Object> getter, Handle<Object> setter,
    PropertyAttributes attributes) {
  DCHECK(!getter->IsNull(isolate_) || !setter->IsNull(isolate_));
  // Can only be called when the receiver is a JSObject. JSProxy has to be
  // handled via a trap. Adding properties to primitive values is not
  // observable.
  Handle<JSObject> receiver = GetStoreTarget<JSObject>();
  if (!IsElement() && name()->IsPrivate(isolate_)) {
    attributes = static_cast<PropertyAttributes>(attributes | DONT_ENUM);
  }

  if (!IsElement(*receiver) && !receiver->map(isolate_).is_dictionary_map()) {
    Handle<Map> old_map(receiver->map(isolate_), isolate_);

    if (!holder_.is_identical_to(receiver)) {
      holder_ = receiver;
      state_ = NOT_FOUND;
    } else if (state_ == INTERCEPTOR) {
      LookupInRegularHolder<false>(*old_map, *holder_);
    }
    // The case of IsFound() && number_.is_not_found() can occur for
    // interceptors.
    DCHECK_IMPLIES(!IsFound(), number_.is_not_found());

    Handle<Map> new_map = Map::TransitionToAccessorProperty(
        isolate_, old_map, name_, number_, getter, setter, attributes);
    bool simple_transition =
        new_map->GetBackPointer(isolate_) == receiver->map(isolate_);
    JSObject::MigrateToMap(isolate_, receiver, new_map);

    if (simple_transition) {
      number_ = new_map->LastAdded();
      property_details_ = new_map->GetLastDescriptorDetails(isolate_);
      state_ = ACCESSOR;
      return;
    }

    ReloadPropertyInformation<false>();
    if (!new_map->is_dictionary_map()) return;
  }

  Handle<AccessorPair> pair;
  if (state() == ACCESSOR && GetAccessors()->IsAccessorPair(isolate_)) {
    pair = Handle<AccessorPair>::cast(GetAccessors());
    // If the component and attributes are identical, nothing has to be done.
    if (pair->Equals(*getter, *setter)) {
      if (property_details().attributes() == attributes) {
        if (!IsElement(*receiver)) JSObject::ReoptimizeIfPrototype(receiver);
        return;
      }
    } else {
      pair = AccessorPair::Copy(isolate(), pair);
      pair->SetComponents(*getter, *setter);
    }
  } else {
    pair = factory()->NewAccessorPair();
    pair->SetComponents(*getter, *setter);
  }

  TransitionToAccessorPair(pair, attributes);

#if VERIFY_HEAP
  if (v8_flags.verify_heap) {
    receiver->JSObjectVerify(isolate());
  }
#endif
}

void LookupIterator::TransitionToAccessorPair(Handle<Object> pair,
                                              PropertyAttributes attributes) {
  Handle<JSObject> receiver = GetStoreTarget<JSObject>();
  holder_ = receiver;

  PropertyDetails details(PropertyKind::kAccessor, attributes,
                          PropertyCellType::kMutable);

  if (IsElement(*receiver)) {
    // TODO(verwaest): Move code into the element accessor.
    isolate_->CountUsage(v8::Isolate::kIndexAccessor);
    Handle<NumberDictionary> dictionary = JSObject::NormalizeElements(receiver);

    dictionary = NumberDictionary::Set(isolate_, dictionary, array_index(),
                                       pair, receiver, details);
    receiver->RequireSlowElements(*dictionary);

    if (receiver->HasSlowArgumentsElements(isolate_)) {
      SloppyArgumentsElements parameter_map =
          SloppyArgumentsElements::cast(receiver->elements(isolate_));
      uint32_t length = parameter_map.length();
      if (number_.is_found() && number_.as_uint32() < length) {
        parameter_map.set_mapped_entries(
            number_.as_int(), ReadOnlyRoots(isolate_).the_hole_value());
      }
      parameter_map.set_arguments(*dictionary);
    } else {
      receiver->set_elements(*dictionary);
    }

    ReloadPropertyInformation<true>();
  } else {
    PropertyNormalizationMode mode = CLEAR_INOBJECT_PROPERTIES;
    if (receiver->map(isolate_).is_prototype_map()) {
      JSObject::InvalidatePrototypeChains(receiver->map(isolate_));
      mode = KEEP_INOBJECT_PROPERTIES;
    }

    // Normalize object to make this operation simple.
    JSObject::NormalizeProperties(isolate_, receiver, mode, 0,
                                  "TransitionToAccessorPair");

    JSObject::SetNormalizedProperty(receiver, name_, pair, details);
    JSObject::ReoptimizeIfPrototype(receiver);

    ReloadPropertyInformation<false>();
  }
}

bool LookupIterator::HolderIsReceiver() const {
  DCHECK(has_property_ || state_ == INTERCEPTOR || state_ == JSPROXY);
  // Optimization that only works if configuration_ is not mutable.
  if (!check_prototype_chain()) return true;
  return *receiver_ == *holder_;
}

bool LookupIterator::HolderIsReceiverOrHiddenPrototype() const {
  DCHECK(has_property_ || state_ == INTERCEPTOR || state_ == JSPROXY);
  // Optimization that only works if configuration_ is not mutable.
  if (!check_prototype_chain()) return true;
  if (*receiver_ == *holder_) return true;
  if (!receiver_->IsJSGlobalProxy(isolate_)) return false;
  return Handle<JSGlobalProxy>::cast(receiver_)->map(isolate_).prototype(
             isolate_) == *holder_;
}

Handle<Object> LookupIterator::FetchValue(
    AllocationPolicy allocation_policy) const {
  Object result;
  DCHECK(!holder_->IsWasmObject());
  if (IsElement(*holder_)) {
    Handle<JSObject> holder = GetHolder<JSObject>();
    ElementsAccessor* accessor = holder->GetElementsAccessor(isolate_);
    return accessor->Get(isolate_, holder, number_);
  } else if (holder_->IsJSGlobalObject(isolate_)) {
    Handle<JSGlobalObject> holder = GetHolder<JSGlobalObject>();
    result = holder->global_dictionary(isolate_, kAcquireLoad)
                 .ValueAt(isolate_, dictionary_entry());
  } else if (!holder_->HasFastProperties(isolate_)) {
    if constexpr (V8_ENABLE_SWISS_NAME_DICTIONARY_BOOL) {
      result = holder_->property_dictionary_swiss(isolate_).ValueAt(
          dictionary_entry());
    } else {
      result = holder_->property_dictionary(isolate_).ValueAt(
          isolate_, dictionary_entry());
    }
  } else if (property_details_.location() == PropertyLocation::kField) {
    DCHECK_EQ(PropertyKind::kData, property_details_.kind());
    Handle<JSObject> holder = GetHolder<JSObject>();
    FieldIndex field_index =
        FieldIndex::ForDetails(holder->map(isolate_), property_details_);
    if (allocation_policy == AllocationPolicy::kAllocationDisallowed &&
        field_index.is_inobject() && field_index.is_double()) {
      return isolate_->factory()->undefined_value();
    }
    return JSObject::FastPropertyAt(
        isolate_, holder, property_details_.representation(), field_index);
  } else {
    result =
        holder_->map(isolate_).instance_descriptors(isolate_).GetStrongValue(
            isolate_, descriptor_number());
  }
  return handle(result, isolate_);
}

bool LookupIterator::CanStayConst(Object value) const {
  DCHECK(!holder_.is_null());
  DCHECK(!IsElement(*holder_));
  DCHECK(holder_->HasFastProperties(isolate_));
  DCHECK_EQ(PropertyLocation::kField, property_details_.location());
  DCHECK_EQ(PropertyConstness::kConst, property_details_.constness());
  if (value.IsUninitialized(isolate())) {
    // Storing uninitialized value means that we are preparing for a computed
    // property value in an object literal. The initializing store will follow
    // and it will properly update constness based on the actual value.
    return true;
  }
  Handle<JSObject> holder = GetHolder<JSObject>();
  FieldIndex field_index =
      FieldIndex::ForDetails(holder->map(isolate_), property_details_);
  if (property_details_.representation().IsDouble()) {
    if (!value.IsNumber(isolate_)) return false;
    uint64_t bits;
    Object current_value = holder->RawFastPropertyAt(isolate_, field_index);
    DCHECK(current_value.IsHeapNumber(isolate_));
    bits = HeapNumber::cast(current_value).value_as_bits(kRelaxedLoad);
    // Use bit representation of double to check for hole double, since
    // manipulating the signaling NaN used for the hole in C++, e.g. with
    // base::bit_cast or value(), will change its value on ia32 (the x87
    // stack is used to return values and stores to the stack silently clear the
    // signalling bit).
    // Only allow initializing stores to double to stay constant.
    return bits == kHoleNanInt64;
  }

  Object current_value = holder->RawFastPropertyAt(isolate_, field_index);
  return current_value.IsUninitialized(isolate());
}

bool LookupIterator::DictCanStayConst(Object value) const {
  DCHECK(!holder_.is_null());
  DCHECK(!IsElement(*holder_));
  DCHECK(!holder_->HasFastProperties(isolate_));
  DCHECK(!holder_->IsJSGlobalObject());
  DCHECK(!holder_->IsJSProxy());
  DCHECK_EQ(PropertyConstness::kConst, property_details_.constness());

  DisallowHeapAllocation no_gc;

  if (value.IsUninitialized(isolate())) {
    // Storing uninitialized value means that we are preparing for a computed
    // property value in an object literal. The initializing store will follow
    // and it will properly update constness based on the actual value.
    return true;
  }
  Handle<JSReceiver> holder = GetHolder<JSReceiver>();
  Object current_value;
  if constexpr (V8_ENABLE_SWISS_NAME_DICTIONARY_BOOL) {
    SwissNameDictionary dict = holder->property_dictionary_swiss();
    current_value = dict.ValueAt(dictionary_entry());
  } else {
    NameDictionary dict = holder->property_dictionary();
    current_value = dict.ValueAt(dictionary_entry());
  }

  return current_value.IsUninitialized(isolate());
}

int LookupIterator::GetFieldDescriptorIndex() const {
  DCHECK(has_property_);
  DCHECK(holder_->HasFastProperties());
  DCHECK_EQ(PropertyLocation::kField, property_details_.location());
  DCHECK_EQ(PropertyKind::kData, property_details_.kind());
  // TODO(jkummerow): Propagate InternalIndex further.
  return descriptor_number().as_int();
}

int LookupIterator::GetAccessorIndex() const {
  DCHECK(has_property_);
  DCHECK(holder_->HasFastProperties(isolate_));
  DCHECK_EQ(PropertyLocation::kDescriptor, property_details_.location());
  DCHECK_EQ(PropertyKind::kAccessor, property_details_.kind());
  return descriptor_number().as_int();
}

FieldIndex LookupIterator::GetFieldIndex() const {
  DCHECK(has_property_);
  DCHECK(!holder_.is_null());
  DCHECK(holder_->HasFastProperties(isolate_));
  DCHECK_EQ(PropertyLocation::kField, property_details_.location());
  DCHECK(!IsElement(*holder_));
  return FieldIndex::ForDetails(holder_->map(isolate_), property_details_);
}

Handle<PropertyCell> LookupIterator::GetPropertyCell() const {
  DCHECK(!holder_.is_null());
  DCHECK(!IsElement(*holder_));
  Handle<JSGlobalObject> holder = GetHolder<JSGlobalObject>();
  return handle(holder->global_dictionary(isolate_, kAcquireLoad)
                    .CellAt(isolate_, dictionary_entry()),
                isolate_);
}

Handle<Object> LookupIterator::GetAccessors() const {
  DCHECK_EQ(ACCESSOR, state_);
  return FetchValue();
}

Handle<Object> LookupIterator::GetDataValue(
    AllocationPolicy allocation_policy) const {
  DCHECK_EQ(DATA, state_);
  Handle<Object> value = FetchValue(allocation_policy);
  return value;
}

Handle<Object> LookupIterator::GetDataValue(SeqCstAccessTag tag) const {
  DCHECK_EQ(DATA, state_);
  // Currently only shared structs and arrays support sequentially consistent
  // access.
  DCHECK(holder_->IsJSSharedStruct(isolate_) ||
         holder_->IsJSSharedArray(isolate_));
  Handle<JSObject> holder = GetHolder<JSObject>();
  if (IsElement(*holder)) {
    ElementsAccessor* accessor = holder->GetElementsAccessor(isolate_);
    return accessor->GetAtomic(isolate_, holder, number_, kSeqCstAccess);
  }
  DCHECK_EQ(PropertyLocation::kField, property_details_.location());
  DCHECK_EQ(PropertyKind::kData, property_details_.kind());
  FieldIndex field_index =
      FieldIndex::ForDetails(holder->map(isolate_), property_details_);
  return JSObject::FastPropertyAt(
      isolate_, holder, property_details_.representation(), field_index, tag);
}

void LookupIterator::WriteDataValue(Handle<Object> value,
                                    bool initializing_store) {
  DCHECK_EQ(DATA, state_);
  // WriteDataValueToWasmObject() must be used instead for writing to
  // WasmObjects.
  DCHECK(!holder_->IsWasmObject(isolate_));
  DCHECK_IMPLIES(holder_->IsJSSharedStruct(), value->IsShared());

  Handle<JSReceiver> holder = GetHolder<JSReceiver>();
  if (IsElement(*holder)) {
    Handle<JSObject> object = Handle<JSObject>::cast(holder);
    ElementsAccessor* accessor = object->GetElementsAccessor(isolate_);
    accessor->Set(object, number_, *value);
  } else if (holder->HasFastProperties(isolate_)) {
    DCHECK(holder->IsJSObject(isolate_));
    if (property_details_.location() == PropertyLocation::kField) {
      // Check that in case of VariableMode::kConst field the existing value is
      // equal to |value|.
      DCHECK_IMPLIES(!initializing_store && property_details_.constness() ==
                                                PropertyConstness::kConst,
                     CanStayConst(*value));
      JSObject::cast(*holder).WriteToField(descriptor_number(),
                                           property_details_, *value);
    } else {
      DCHECK_EQ(PropertyLocation::kDescriptor, property_details_.location());
      DCHECK_EQ(PropertyConstness::kConst, property_details_.constness());
    }
  } else if (holder->IsJSGlobalObject(isolate_)) {
    // PropertyCell::PrepareForAndSetValue already wrote the value into the
    // cell.
#ifdef DEBUG
    GlobalDictionary dictionary =
        JSGlobalObject::cast(*holder).global_dictionary(isolate_, kAcquireLoad);
    PropertyCell cell = dictionary.CellAt(isolate_, dictionary_entry());
    DCHECK(cell.value() == *value ||
           (cell.value().IsString() && value->IsString() &&
            String::cast(cell.value()).Equals(String::cast(*value))));
#endif  // DEBUG
  } else {
    DCHECK_IMPLIES(holder->IsJSProxy(isolate_), name()->IsPrivate(isolate_));
    // Check similar to fast mode case above.
    DCHECK_IMPLIES(
        V8_DICT_PROPERTY_CONST_TRACKING_BOOL && !initializing_store &&
            property_details_.constness() == PropertyConstness::kConst,
        holder->IsJSProxy(isolate_) || DictCanStayConst(*value));

    if constexpr (V8_ENABLE_SWISS_NAME_DICTIONARY_BOOL) {
      SwissNameDictionary dictionary =
          holder->property_dictionary_swiss(isolate_);
      dictionary.ValueAtPut(dictionary_entry(), *value);
    } else {
      NameDictionary dictionary = holder->property_dictionary(isolate_);
      dictionary.ValueAtPut(dictionary_entry(), *value);
    }
  }
}

void LookupIterator::WriteDataValue(Handle<Object> value, SeqCstAccessTag tag) {
  DCHECK_EQ(DATA, state_);
  // Currently only shared structs and arrays support sequentially consistent
  // access.
  DCHECK(holder_->IsJSSharedStruct(isolate_) ||
         holder_->IsJSSharedArray(isolate_));
  Handle<JSObject> holder = GetHolder<JSObject>();
  if (IsElement(*holder)) {
    ElementsAccessor* accessor = holder->GetElementsAccessor(isolate_);
    accessor->SetAtomic(holder, number_, *value, kSeqCstAccess);
    return;
  }
  DCHECK_EQ(PropertyLocation::kField, property_details_.location());
  DCHECK_EQ(PropertyKind::kData, property_details_.kind());
  DisallowGarbageCollection no_gc;
  FieldIndex field_index =
      FieldIndex::ForDescriptor(holder->map(isolate_), descriptor_number());
  holder->FastPropertyAtPut(field_index, *value, tag);
}

Handle<Object> LookupIterator::SwapDataValue(Handle<Object> value,
                                             SeqCstAccessTag tag) {
  DCHECK_EQ(DATA, state_);
  // Currently only shared structs and arrays support sequentially consistent
  // access.
  DCHECK(holder_->IsJSSharedStruct(isolate_) ||
         holder_->IsJSSharedArray(isolate_));
  Handle<JSObject> holder = GetHolder<JSObject>();
  if (IsElement(*holder)) {
    ElementsAccessor* accessor = holder->GetElementsAccessor(isolate_);
    return accessor->SwapAtomic(isolate_, holder, number_, *value,
                                kSeqCstAccess);
  }
  DCHECK_EQ(PropertyLocation::kField, property_details_.location());
  DCHECK_EQ(PropertyKind::kData, property_details_.kind());
  DisallowGarbageCollection no_gc;
  FieldIndex field_index =
      FieldIndex::ForDescriptor(holder->map(isolate_), descriptor_number());
  return handle(holder->RawFastPropertyAtSwap(field_index, *value, tag),
                isolate_);
}

template <bool is_element>
bool LookupIterator::SkipInterceptor(JSObject holder) {
  InterceptorInfo info = GetInterceptor<is_element>(holder);
  if (!is_element && name_->IsSymbol(isolate_) &&
      !info.can_intercept_symbols()) {
    return true;
  }
  if (info.non_masking()) {
    switch (interceptor_state_) {
      case InterceptorState::kUninitialized:
        interceptor_state_ = InterceptorState::kSkipNonMasking;
        V8_FALLTHROUGH;
      case InterceptorState::kSkipNonMasking:
        return true;
      case InterceptorState::kProcessNonMasking:
        return false;
    }
  }
  return interceptor_state_ == InterceptorState::kProcessNonMasking;
}

JSReceiver LookupIterator::NextHolder(Map map) {
  DisallowGarbageCollection no_gc;
  if (map.prototype(isolate_) == ReadOnlyRoots(isolate_).null_value()) {
    return JSReceiver();
  }
  if (!check_prototype_chain() && !map.IsJSGlobalProxyMap()) {
    return JSReceiver();
  }
  return JSReceiver::cast(map.prototype(isolate_));
}

LookupIterator::State LookupIterator::NotFound(JSReceiver const holder) const {
  if (!holder.IsJSTypedArray(isolate_)) return NOT_FOUND;
  if (IsElement()) return INTEGER_INDEXED_EXOTIC;
  if (!name_->IsString(isolate_)) return NOT_FOUND;
  return IsSpecialIndex(String::cast(*name_)) ? INTEGER_INDEXED_EXOTIC
                                              : NOT_FOUND;
}

namespace {

template <bool is_element>
bool HasInterceptor(Map map, size_t index) {
  if (is_element) {
    if (index > JSObject::kMaxElementIndex) {
      // There is currently no way to install interceptors on an object with
      // typed array elements.
      DCHECK(!map.has_typed_array_or_rab_gsab_typed_array_elements());
      return map.has_named_interceptor();
    }
    return map.has_indexed_interceptor();
  } else {
    return map.has_named_interceptor();
  }
}

}  // namespace

template <bool is_element>
LookupIterator::State LookupIterator::LookupInSpecialHolder(
    Map const map, JSReceiver const holder) {
  static_assert(INTERCEPTOR == BEFORE_PROPERTY);
  switch (state_) {
    case NOT_FOUND:
      if (map.IsJSProxyMap()) {
        if (is_element || !name_->IsPrivate(isolate_)) return JSPROXY;
      }
#if V8_ENABLE_WEBASSEMBLY
      if (map.IsWasmObjectMap()) return WASM_OBJECT;
#endif  // V8_ENABLE_WEBASSEMBLY
      if (map.is_access_check_needed()) {
        if (is_element || !name_->IsPrivate(isolate_) ||
            name_->IsPrivateName(isolate_))
          return ACCESS_CHECK;
      }
      V8_FALLTHROUGH;
    case ACCESS_CHECK:
      if (check_interceptor() && HasInterceptor<is_element>(map, index_) &&
          !SkipInterceptor<is_element>(JSObject::cast(holder))) {
        if (is_element || !name_->IsPrivate(isolate_)) return INTERCEPTOR;
      }
      V8_FALLTHROUGH;
    case INTERCEPTOR:
      if (map.IsJSGlobalObjectMap() && !is_js_array_element(is_element)) {
        GlobalDictionary dict = JSGlobalObject::cast(holder).global_dictionary(
            isolate_, kAcquireLoad);
        number_ = dict.FindEntry(isolate(), name_);
        if (number_.is_not_found()) return NOT_FOUND;
        PropertyCell cell = dict.CellAt(isolate_, number_);
        if (cell.value(isolate_).IsTheHole(isolate_)) {
          return NOT_FOUND;
        }
        property_details_ = cell.property_details();
        has_property_ = true;
        switch (property_details_.kind()) {
          case v8::internal::PropertyKind::kData:
            return DATA;
          case v8::internal::PropertyKind::kAccessor:
            return ACCESSOR;
        }
      }
      return LookupInRegularHolder<is_element>(map, holder);
    case ACCESSOR:
    case DATA:
      return NOT_FOUND;
    case INTEGER_INDEXED_EXOTIC:
    case JSPROXY:
    case WASM_OBJECT:
    case TRANSITION:
      UNREACHABLE();
  }
  UNREACHABLE();
}

template <bool is_element>
LookupIterator::State LookupIterator::LookupInRegularHolder(
    Map const map, JSReceiver const holder) {
  DisallowGarbageCollection no_gc;
  if (interceptor_state_ == InterceptorState::kProcessNonMasking) {
    return NOT_FOUND;
  }
  DCHECK(!holder.IsWasmObject(isolate_));
  if (is_element && IsElement(holder)) {
    JSObject js_object = JSObject::cast(holder);
    ElementsAccessor* accessor = js_object.GetElementsAccessor(isolate_);
    FixedArrayBase backing_store = js_object.elements(isolate_);
    number_ =
        accessor->GetEntryForIndex(isolate_, js_object, backing_store, index_);
    if (number_.is_not_found()) {
      return holder.IsJSTypedArray(isolate_) ? INTEGER_INDEXED_EXOTIC
                                             : NOT_FOUND;
    }
    property_details_ = accessor->GetDetails(js_object, number_);
    if (map.has_frozen_elements()) {
      property_details_ = property_details_.CopyAddAttributes(FROZEN);
    } else if (map.has_sealed_elements()) {
      property_details_ = property_details_.CopyAddAttributes(SEALED);
    }
  } else if (!map.is_dictionary_map()) {
    DescriptorArray descriptors = map.instance_descriptors(isolate_);
    number_ = descriptors.SearchWithCache(isolate_, *name_, map);
    if (number_.is_not_found()) return NotFound(holder);
    property_details_ = descriptors.GetDetails(number_);
  } else {
    DCHECK_IMPLIES(holder.IsJSProxy(isolate_), name()->IsPrivate(isolate_));
    if constexpr (V8_ENABLE_SWISS_NAME_DICTIONARY_BOOL) {
      SwissNameDictionary dict = holder.property_dictionary_swiss(isolate_);
      number_ = dict.FindEntry(isolate(), *name_);
      if (number_.is_not_found()) return NotFound(holder);
      property_details_ = dict.DetailsAt(number_);
    } else {
      NameDictionary dict = holder.property_dictionary(isolate_);
      number_ = dict.FindEntry(isolate(), name_);
      if (number_.is_not_found()) return NotFound(holder);
      property_details_ = dict.DetailsAt(number_);
    }
  }
  has_property_ = true;
  switch (property_details_.kind()) {
    case v8::internal::PropertyKind::kData:
      return DATA;
    case v8::internal::PropertyKind::kAccessor:
      return ACCESSOR;
  }

  UNREACHABLE();
}

Handle<InterceptorInfo> LookupIterator::GetInterceptorForFailedAccessCheck()
    const {
  DCHECK_EQ(ACCESS_CHECK, state_);
  // Skip the interceptors for private
  if (IsPrivateName()) {
    return Handle<InterceptorInfo>();
  }

  DisallowGarbageCollection no_gc;
  AccessCheckInfo access_check_info =
      AccessCheckInfo::Get(isolate_, Handle<JSObject>::cast(holder_));
  if (!access_check_info.is_null()) {
    // There is currently no way to create objects with typed array elements
    // and access checks.
    DCHECK(!holder_->map().has_typed_array_or_rab_gsab_typed_array_elements());
    Object interceptor = is_js_array_element(IsElement())
                             ? access_check_info.indexed_interceptor()
                             : access_check_info.named_interceptor();
    if (interceptor != Object()) {
      return handle(InterceptorInfo::cast(interceptor), isolate_);
    }
  }
  return Handle<InterceptorInfo>();
}

bool LookupIterator::TryLookupCachedProperty(Handle<AccessorPair> accessor) {
  DCHECK_EQ(state(), LookupIterator::ACCESSOR);
  return LookupCachedProperty(accessor);
}

bool LookupIterator::TryLookupCachedProperty() {
  if (state() != LookupIterator::ACCESSOR) return false;

  Handle<Object> accessor_pair = GetAccessors();
  return accessor_pair->IsAccessorPair(isolate_) &&
         LookupCachedProperty(Handle<AccessorPair>::cast(accessor_pair));
}

bool LookupIterator::LookupCachedProperty(Handle<AccessorPair> accessor_pair) {
  if (!HolderIsReceiverOrHiddenPrototype()) return false;
  if (!lookup_start_object_.is_identical_to(receiver_) &&
      !lookup_start_object_.is_identical_to(holder_)) {
    return false;
  }

  DCHECK_EQ(state(), LookupIterator::ACCESSOR);
  DCHECK(GetAccessors()->IsAccessorPair(isolate_));

  Object getter = accessor_pair->getter(isolate_);
  base::Optional<Name> maybe_name =
      FunctionTemplateInfo::TryGetCachedPropertyName(isolate(), getter);
  if (!maybe_name.has_value()) return false;

  if (getter.IsJSFunction()) {
    // If the getter was a JSFunction there's no guarantee that the holder
    // actually has a property with the cached name. In that case look it up to
    // make sure.
    LookupIterator it(isolate_, holder_, handle(maybe_name.value(), isolate_));
    if (it.state() != DATA) return false;
    name_ = it.name();
  } else {
    name_ = handle(maybe_name.value(), isolate_);
  }

  // We have found a cached property! Modify the iterator accordingly.
  Restart();
  CHECK_EQ(state(), LookupIterator::DATA);
  return true;
}

// static
base::Optional<Object> ConcurrentLookupIterator::TryGetOwnCowElement(
    Isolate* isolate, FixedArray array_elements, ElementsKind elements_kind,
    int array_length, size_t index) {
  DisallowGarbageCollection no_gc;

  CHECK_EQ(array_elements.map(), ReadOnlyRoots(isolate).fixed_cow_array_map());
  DCHECK(IsFastElementsKind(elements_kind) &&
         IsSmiOrObjectElementsKind(elements_kind));
  USE(elements_kind);
  DCHECK_GE(array_length, 0);

  //  ________________________________________
  // ( Check against both JSArray::length and )
  // ( FixedArray::length.                    )
  //  ----------------------------------------
  //         o   ^__^
  //          o  (oo)\_______
  //             (__)\       )\/\
  //                 ||----w |
  //                 ||     ||
  // The former is the source of truth, but due to concurrent reads it may not
  // match the given `array_elements`.
  if (index >= static_cast<size_t>(array_length)) return {};
  if (index >= static_cast<size_t>(array_elements.length())) return {};

  Object result = array_elements.get(isolate, static_cast<int>(index));

  //  ______________________________________
  // ( Filter out holes irrespective of the )
  // ( elements kind.                       )
  //  --------------------------------------
  //         o   ^__^
  //          o  (..)\_______
  //             (__)\       )\/\
  //                 ||----w |
  //                 ||     ||
  // The elements kind may not be consistent with the given elements backing
  // store.
  if (result == ReadOnlyRoots(isolate).the_hole_value()) return {};

  return result;
}

// static
ConcurrentLookupIterator::Result
ConcurrentLookupIterator::TryGetOwnConstantElement(
    Object* result_out, Isolate* isolate, LocalIsolate* local_isolate,
    JSObject holder, FixedArrayBase elements, ElementsKind elements_kind,
    size_t index) {
  DisallowGarbageCollection no_gc;

  DCHECK_LE(index, JSObject::kMaxElementIndex);

  // Own 'constant' elements (PropertyAttributes READ_ONLY|DONT_DELETE) occur in
  // three main cases:
  //
  // 1. Frozen elements: guaranteed constant.
  // 2. Dictionary elements: may be constant.
  // 3. String wrapper elements: guaranteed constant.

  // Interesting field reads below:
  //
  // - elements.length (immutable on FixedArrays).
  // - elements[i] (immutable if constant; be careful around dictionaries).
  // - holder.AsJSPrimitiveWrapper.value.AsString.length (immutable).
  // - holder.AsJSPrimitiveWrapper.value.AsString[i] (immutable).
  // - single_character_string_table()->get().

  if (IsFrozenElementsKind(elements_kind)) {
    if (!elements.IsFixedArray()) return kGaveUp;
    FixedArray elements_fixed_array = FixedArray::cast(elements);
    if (index >= static_cast<uint32_t>(elements_fixed_array.length())) {
      return kGaveUp;
    }
    Object result = elements_fixed_array.get(isolate, static_cast<int>(index));
    if (IsHoleyElementsKindForRead(elements_kind) &&
        result == ReadOnlyRoots(isolate).the_hole_value()) {
      return kNotPresent;
    }
    *result_out = result;
    return kPresent;
  } else if (IsDictionaryElementsKind(elements_kind)) {
    if (!elements.IsNumberDictionary()) return kGaveUp;
    // TODO(jgruber, v8:7790): Add support. Dictionary elements require racy
    // NumberDictionary lookups. This should be okay in general (slot iteration
    // depends only on the dict's capacity), but 1. we'd need to update
    // NumberDictionary methods to do atomic reads, and 2. the dictionary
    // elements case isn't very important for callers of this function.
    return kGaveUp;
  } else if (IsStringWrapperElementsKind(elements_kind)) {
    // In this case we don't care about the actual `elements`. All in-bounds
    // reads are redirected to the wrapped String.

    JSPrimitiveWrapper js_value = JSPrimitiveWrapper::cast(holder);
    String wrapped_string = String::cast(js_value.value());
    return ConcurrentLookupIterator::TryGetOwnChar(
        static_cast<String*>(result_out), isolate, local_isolate,
        wrapped_string, index);
  } else {
    DCHECK(!IsFrozenElementsKind(elements_kind));
    DCHECK(!IsDictionaryElementsKind(elements_kind));
    DCHECK(!IsStringWrapperElementsKind(elements_kind));
    return kGaveUp;
  }

  UNREACHABLE();
}

// static
ConcurrentLookupIterator::Result ConcurrentLookupIterator::TryGetOwnChar(
    String* result_out, Isolate* isolate, LocalIsolate* local_isolate,
    String string, size_t index) {
  DisallowGarbageCollection no_gc;
  // The access guard below protects string accesses related to internalized
  // strings.
  // TODO(jgruber): Support other string kinds.
  Map string_map = string.map(isolate, kAcquireLoad);
  InstanceType type = string_map.instance_type();
  if (!(InstanceTypeChecker::IsInternalizedString(type) ||
        InstanceTypeChecker::IsThinString(type))) {
    return kGaveUp;
  }

  const uint32_t length = static_cast<uint32_t>(string.length());
  if (index >= length) return kGaveUp;

  uint16_t charcode;
  {
    SharedStringAccessGuardIfNeeded access_guard(local_isolate);
    charcode = string.Get(static_cast<int>(index), PtrComprCageBase(isolate),
                          access_guard);
  }

  if (charcode > unibrow::Latin1::kMaxChar) return kGaveUp;

  Object value = isolate->factory()->single_character_string_table()->get(
      charcode, kRelaxedLoad);

  DCHECK_NE(value, ReadOnlyRoots(isolate).undefined_value());

  *result_out = String::cast(value);
  return kPresent;
}

// static
base::Optional<PropertyCell> ConcurrentLookupIterator::TryGetPropertyCell(
    Isolate* isolate, LocalIsolate* local_isolate,
    Handle<JSGlobalObject> holder, Handle<Name> name) {
  DisallowGarbageCollection no_gc;

  Map holder_map = holder->map();
  if (holder_map.is_access_check_needed()) return {};
  if (holder_map.has_named_interceptor()) return {};

  GlobalDictionary dict = holder->global_dictionary(kAcquireLoad);
  base::Optional<PropertyCell> cell =
      dict.TryFindPropertyCellForConcurrentLookupIterator(isolate, name,
                                                          kRelaxedLoad);
  if (!cell.has_value()) return {};

  if (cell->property_details(kAcquireLoad).kind() == PropertyKind::kAccessor) {
    Object maybe_accessor_pair = cell->value(kAcquireLoad);
    if (!maybe_accessor_pair.IsAccessorPair()) return {};

    base::Optional<Name> maybe_cached_property_name =
        FunctionTemplateInfo::TryGetCachedPropertyName(
            isolate, AccessorPair::cast(maybe_accessor_pair)
                         .getter(isolate, kAcquireLoad));
    if (!maybe_cached_property_name.has_value()) return {};

    cell = dict.TryFindPropertyCellForConcurrentLookupIterator(
        isolate, handle(*maybe_cached_property_name, local_isolate),
        kRelaxedLoad);
    if (!cell.has_value()) return {};
    if (cell->property_details(kAcquireLoad).kind() != PropertyKind::kData)
      return {};
  }

  DCHECK(cell.has_value());
  DCHECK_EQ(cell->property_details(kAcquireLoad).kind(), PropertyKind::kData);
  return cell;
}

}  // namespace internal
}  // namespace v8
