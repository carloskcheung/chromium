// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/css/properties/shorthands/margin.h"

#include "third_party/blink/renderer/core/css/parser/css_property_parser_helpers.h"
#include "third_party/blink/renderer/core/css/properties/computed_style_utils.h"
#include "third_party/blink/renderer/core/layout/layout_object.h"
#include "third_party/blink/renderer/core/style/computed_style.h"
#include "third_party/blink/renderer/core/style_property_shorthand.h"

namespace blink {
namespace CSSShorthand {

bool Margin::ParseShorthand(
    bool important,
    CSSParserTokenRange& range,
    const CSSParserContext& context,
    const CSSParserLocalContext&,
    HeapVector<CSSPropertyValue, 256>& properties) const {
  return css_property_parser_helpers::ConsumeShorthandVia4Longhands(
      marginShorthand(), important, context, range, properties);
}

bool Margin::IsLayoutDependent(const ComputedStyle* style,
                               LayoutObject* layout_object) const {
  return layout_object && layout_object->IsBox() &&
         (!style || !style->MarginBottom().IsFixed() ||
          !style->MarginTop().IsFixed() || !style->MarginLeft().IsFixed() ||
          !style->MarginRight().IsFixed());
}

const CSSValue* Margin::CSSValueFromComputedStyleInternal(
    const ComputedStyle& style,
    const SVGComputedStyle&,
    const LayoutObject* layout_object,
    Node* styled_node,
    bool allow_visited_style) const {
  return ComputedStyleUtils::ValuesForSidesShorthand(marginShorthand(), style,
                                                     layout_object, styled_node,
                                                     allow_visited_style);
}

}  // namespace CSSShorthand
}  // namespace blink
