// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/css/properties/longhands/grid_template_rows.h"

#include "third_party/blink/renderer/core/css/parser/css_parser_context.h"
#include "third_party/blink/renderer/core/css/properties/computed_style_utils.h"
#include "third_party/blink/renderer/core/css/properties/css_parsing_utils.h"
#include "third_party/blink/renderer/core/layout/layout_object.h"
#include "third_party/blink/renderer/core/style/computed_style.h"

namespace blink {
namespace CSSLonghand {

const CSSValue* GridTemplateRows::ParseSingleValue(
    CSSParserTokenRange& range,
    const CSSParserContext& context,
    const CSSParserLocalContext&) const {
  return css_parsing_utils::ConsumeGridTemplatesRowsOrColumns(range, context,
                                                              context.Mode());
}

bool GridTemplateRows::IsLayoutDependent(const ComputedStyle* style,
                                         LayoutObject* layout_object) const {
  return layout_object && layout_object->IsLayoutGrid();
}

const CSSValue* GridTemplateRows::CSSValueFromComputedStyleInternal(
    const ComputedStyle& style,
    const SVGComputedStyle&,
    const LayoutObject* layout_object,
    Node* styled_node,
    bool allow_visited_style) const {
  return ComputedStyleUtils::ValueForGridTrackList(kForRows, layout_object,
                                                   style);
}

}  // namespace CSSLonghand
}  // namespace blink
