# Match complete OSense sections and boolean switch items, preserving other bytes.
function attribute(key, line,   pattern) {
    pattern = "[[:space:]]" key "=\"[^\"]*\""
    if (!match(line, pattern)) return ""
    return substr(line, RSTART+length(key)+3, RLENGTH-length(key)-4)
}
BEGIN { supported["swap"] = supported["dynaZramConfig"] = supported["cMemConfig"] = 1 }
/^[[:space:]]*<config[[:space:]]/ {
    section = attribute("name", $0)
    if (section in supported) { sections[section]++; count++ }
}
/^[[:space:]]*<\/config>/ { section = "" }
(section in supported) && /^[[:space:]]*<item[[:space:]]/ && attribute("name", $0) == "switch" {
    v = attribute("value", $0)
    if (attribute("type",$0) != "bool" || (v != "true" && v != "false")) invalid = 1
    switches[section]++
    sub(/value="true"/, "value=\"false\"")
}
{ print }
END {
    for (s in sections) if (sections[s] != 1 || switches[s] != 1) invalid = 1
    if (invalid || !count) exit 1
}
