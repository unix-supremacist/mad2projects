package main

import (
	"fmt"
	"os"
	"regexp"
)

// SaveGraphicsKeys are the <s id="..."> entries this launcher exposes from
// the save directory's Config.xml -- the game's own graphics/resolution
// settings, as opposed to anything mad2config.dll owns. Format observed:
//
//	<r><s id="ScreenWidth">1280</s><s id="ScreenHeight">720</s>
//	   <s id="VideoQuality">2</s><s id="VideoSettingsSet">1</s></r>
var SaveGraphicsKeys = []string{"ScreenWidth", "ScreenHeight", "VideoQuality", "VideoSettingsSet"}

// ResolveSaveDir returns [SaveLoader] SaveDir from cfg (relative to the
// install dir), defaulting to "saves" -- see
// mad2saveloader/src/save_redirect.cpp's own default. cfg may be nil (the
// install's config.cfg hasn't been seeded yet).
func ResolveSaveDir(cfg *ConfigFile) string {
	if cfg != nil {
		if e := cfg.Find("SaveLoader", "SaveDir"); e != nil {
			return e.Value
		}
	}
	return "saves"
}

var saveXmlEntryRe = regexp.MustCompile(`<s id="([^"]+)">([^<]*)</s>`)

// ReadSaveConfigXML parses every <s id="X">value</s> entry in path.
// A missing file returns (nil, nil) -- the save dir may not have been
// written by the game yet.
func ReadSaveConfigXML(path string) (map[string]string, error) {
	data, err := os.ReadFile(path)
	if os.IsNotExist(err) {
		return nil, nil
	}
	if err != nil {
		return nil, err
	}
	values := map[string]string{}
	for _, m := range saveXmlEntryRe.FindAllStringSubmatch(string(data), -1) {
		values[m[1]] = m[2]
	}
	return values, nil
}

// WriteSaveConfigXML rewrites each <s id="key">...</s> entry named in
// updates in place, leaving every other entry and the file's structure
// untouched -- same "surgical regex replace" approach alchemy.xml editing
// uses in ../mad2mod, since this is a tiny fixed-shape file, not worth a
// full XML library for a MinGW-cross-compiled-adjacent tool.
func WriteSaveConfigXML(path string, updates map[string]string) error {
	data, err := os.ReadFile(path)
	if err != nil {
		return err
	}
	content := string(data)
	for key, val := range updates {
		re := regexp.MustCompile(`(<s id="` + regexp.QuoteMeta(key) + `">)[^<]*(</s>)`)
		content = re.ReplaceAllString(content, fmt.Sprintf(`${1}%s${2}`, val))
	}
	return os.WriteFile(path, []byte(content), 0644)
}
