/*
 * This file is part of mpv.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "PGSIdentify.h"

#include <algorithm>
#include <string.h>
#include <string_view>

// Adapted from the mpv subtitle identifier; see PGS-OCR.md for provenance.
static int next_utf8(std::string_view& s)
{
  unsigned c = static_cast<unsigned char>(s.front());
  s.remove_prefix(1);
  if (c < 0x80)
    return c;
  unsigned n = c >= 0xf0 && c < 0xf5   ? 3
               : c >= 0xe0 && c < 0xf0 ? 2
               : c >= 0xc2 && c < 0xe0 ? 1
                                       : 0;
  if (!n || s.size() < n)
    return -1;
  unsigned cp = c & ((1u << (6 - n)) - 1);
  for (unsigned i = 0; i < n; ++i)
  {
    unsigned b = static_cast<unsigned char>(s[i]);
    if ((b & 0xc0) != 0x80)
      return -1;
    cp = (cp << 6) | (b & 63);
  }
  s.remove_prefix(n);
  if ((n == 1 && cp < 0x80) || (n == 2 && cp < 0x800) || (n == 3 && cp < 0x10000) ||
      cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
    return -1;
  return cp;
}
static unsigned char lower_ascii(unsigned char c)
{
  return c >= 'A' && c <= 'Z' ? c + 32 : c;
}
static bool equal_ci(std::string_view a, std::string_view b)
{
  return a.size() == b.size() &&
         std::equal(a.begin(), a.end(), b.begin(), [](unsigned char x, unsigned char y)
                    { return lower_ascii(x) == lower_ascii(y); });
}

// Conservative script markers. Shared/ambiguous forms (后, 台, 只, 干, etc.)
// deliberately do not vote. 拿/照/玩 are shared forms as well;
// 里/着/范 can also be legitimate traditional forms and are not simplified markers.
// We classify observed characters, never transliterate
// OCR output to either script. No dialogue or recognised text is retained.
static const char simplified[] =
    "这说们为来国个会时对还没过让给从开关经应学无现见实认样"
    "请问听话语汉体书车电网门风马龙鸟鱼飞长东叶头点战声乐爱亲"
    "谢欢进远边场岁间满连带队观兴该离变将军传许强师导动机业"
    "广当总则钱万亿买卖办备务劳区卫厂历厉压县发号叹吓团园围"
    "图圆圣坏块坚坛坝执报护担拟拢拥择拨挡挤挥损换据摄敌断"
    "旧显晋晓暂术权条楼树桥欢欧残气汇汤沟泽浅测济浓涂湾湿灭"
    "灯灵灾炉炼热烦烧烟环玛献画疗疯痒皱监盖盘确码礼离种"
    "积称稳签简篮类粮紧红级纪约纯纳纸纹线练组细终结绕络绝统"
    "绿维综继绩绪续绳绘绣绍丝编缘缝缠缩缴罗罚罢联聪肃肠肤脏"
    "脸腾节苏药莹莲蓝获营萧萨虚补装裤规览觉讨记讲诉诊词译"
    "证识试诗诚诞询详误谁调谈谅谋谜谣读课谎谦谨谱贝财责贵贸"
    "费贺资赏赔赚赛赞赵跃践踪转轮软轴轻载轿辈辖输辩达迁迟适"
    "选递遥邮邻郑钟钢铁铜银铭错键镜闪闭闲闻阅阳阴陈阵阶陆险"
    "随隐难雾静顶项顺须顾顿预领颈颊颤颜额颠飘饭饮饱饰馆驱骑"
    "骗骚骂验骄骤齐龄齿"
    // Common unambiguous round-trip pairs checked against OpenCC ST/TS.
    "两严临丽举乌乔习乡争亚产仅众优伞伟伤伦伪侧倾储刘刚创删劝劲势华协单卢厅却厌叠吗吨"
    "启吴员响喷嘱垄垒垦堕壮壳奋奖妇孙审宪寻尘属屡岂岖岗岛币帅帐帜帮庄庆庙庞废异弯弹归"
    "彻忆忧怀态怂怜恳恼悦惊惧惩惫惯";
static const char traditional[] =
    "這說們為來國個會時對裡還沒過讓給從著開關經應學無現見實認樣"
    "請問聽話語漢體書車電網門風馬龍鳥魚飛長東葉頭點戰聲樂愛親"
    "謝歡進遠邊場歲間滿連帶隊觀興該離變將軍傳許強師導動機業"
    "廣當總則錢萬億買賣辦備務勞區衛廠歷厲壓縣發髮號嘆嚇團園圍"
    "圖圓聖壞塊堅壇壩執報護擔擬攏擁擇撥擋擠揮損換據攝敵斷"
    "舊顯晉曉暫術權條樓樹橋歐殘氣匯湯溝澤淺測濟濃塗灣濕滅"
    "燈靈災爐煉熱煩燒煙環瑪獻畫療瘋癢皺監蓋盤確碼禮種"
    "積稱穩簽簡籃類糧緊紅級紀約純納紙紋線練組細終結繞絡絕統"
    "綠維綜繼績緒續繩繪繡紹絲編緣縫纏縮繳羅罰罷聯聰肅腸膚臟"
    "臉騰節範蘇藥瑩蓮藍獲營蕭薩虛補裝褲規覽覺討記講訴診詞譯"
    "證識試詩誠誕詢詳誤誰調談諒謀謎謠讀課謊謙謹譜貝財責貴貿"
    "費賀資賞賠賺賽贊趙躍踐蹤轉輪軟軸輕載轎輩轄輸辯達遷遲適"
    "選遞遙郵鄰鄭鐘鋼鐵銅銀銘錯鍵鏡閃閉閒聞閱陽陰陳陣階陸險"
    "隨隱難霧靜頂項順須顧頓預領頸頰顫顏額顛飄飯飲飽飾館驅騎"
    "騙騷罵驗驕驟齊齡齒"
    // Regional forms corresponding to markers above (OpenCC HK/TW variants).
    "溼爲綫菸衞裏説閲眾偽啟"
    "兩嚴臨麗舉烏喬習鄉爭亞產僅衆優傘偉傷倫僞側傾儲劉剛創刪勸勁勢華協單盧廳卻厭疊嗎噸"
    "啓吳員響噴囑壟壘墾墮壯殼奮獎婦孫審憲尋塵屬屢豈嶇崗島幣帥帳幟幫莊慶廟龐廢異彎彈歸"
    "徹憶憂懷態慫憐懇惱悅驚懼懲憊慣";

static bool contains_cp(const char* text, int cp)
{
  std::string_view s(text);
  while (!s.empty())
  {
    int c = next_utf8(s);
    if (c == cp)
      return true;
  }
  return false;
}

void pgs_sub_script_feed(struct pgs_sub_script_evidence* e, const char* text, int confidence)
{
  if (!text || confidence < 55 || e->confirmed != PGS_SUB_SCRIPT_UNKNOWN)
    return;
  uint64_t hash = 14695981039346656037ULL;
  for (const unsigned char* s = (const unsigned char*)text; *s; s++)
    hash = (hash ^ *s) * 1099511628211ULL;
  for (int n = 0; n < 8; n++)
  {
    if (e->recent[n] == hash)
      return;
  }
  e->recent[e->next_recent++ % 8] = hash;
  e->captions++;
  uint32_t caption_seen[128];
  int num_caption_seen = 0, caption_sim = 0, caption_tra = 0;
  std::string_view s(text);
  while (!s.empty())
  {
    int cp = next_utf8(s);
    if (cp < 0)
    {
      continue;
    }
    bool sim = contains_cp(simplified, cp);
    bool tra = contains_cp(traditional, cp);
    if (sim == tra)
      continue;
    // Count in OCR reading order, across deduplicated captions. Shared
    // characters do not interrupt a run; the opposite script restarts it.
    // Once confirmed, later bilingual sampling must not change the script.
    if (e->confirmed == PGS_SUB_SCRIPT_UNKNOWN)
    {
      enum pgs_sub_script script = sim ? PGS_SUB_SCRIPT_SIMPLIFIED : PGS_SUB_SCRIPT_TRADITIONAL;
      e->run_chars = e->run_script == script ? e->run_chars + 1 : 1;
      e->run_script = script;
      if (e->run_chars >= PGS_SUB_SCRIPT_CONFIRM_CHARS)
        e->confirmed = script;
    }
    // Retain unique-glyph and per-caption counts for diagnostics only.
    // Classification depends solely on the ordered character run above.
    bool caption_repeat = false;
    for (int n = 0; n < num_caption_seen; n++)
      caption_repeat |= caption_seen[n] == static_cast<uint32_t>(cp);
    if (!caption_repeat && num_caption_seen < 128)
    {
      caption_seen[num_caption_seen++] = cp;
      caption_sim += sim;
      caption_tra += tra;
    }
    bool seen = false;
    for (int n = 0; n < e->num_seen; n++)
      seen |= e->seen[n] == static_cast<uint32_t>(cp);
    if (seen || e->num_seen >= 128)
      continue;
    e->seen[e->num_seen++] = cp;
    e->simplified += sim;
    e->traditional += tra;
  }
  e->simplified_captions += caption_sim > 0 && caption_sim >= 2 * caption_tra;
  e->traditional_captions += caption_tra > 0 && caption_tra >= 2 * caption_sim;
  e->last_caption = caption_sim > 0 && caption_sim >= 2 * caption_tra   ? PGS_SUB_SCRIPT_SIMPLIFIED
                    : caption_tra > 0 && caption_tra >= 2 * caption_sim ? PGS_SUB_SCRIPT_TRADITIONAL
                                                                        : PGS_SUB_SCRIPT_UNKNOWN;
}

enum pgs_sub_script pgs_sub_script_result(const struct pgs_sub_script_evidence* e)
{
  return e->confirmed;
}

bool pgs_sub_script_confirm(struct pgs_sub_script_confirmation* s,
                            const struct pgs_sub_script_evidence* e)
{
  s->observed_captions = e->captions;
  s->candidate = e->run_script;
  s->streak = e->run_chars;
  return e->confirmed != PGS_SUB_SCRIPT_UNKNOWN;
}

static bool has_ci(const char* text, const char* needle)
{
  if (!text)
    return false;
  std::string_view s(text), n(needle);
  for (size_t i = 0; i + n.size() <= s.size(); ++i)
  {
    if (equal_ci(s.substr(i, n.size()), n))
      return true;
  }
  return false;
}

static bool ascii_word(unsigned char c)
{
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

static bool token_ci(const char* text, const char* needle)
{
  if (!text)
    return false;
  size_t len = strlen(needle), total = strlen(text);
  for (size_t n = 0; n + len <= total; n++)
  {
    if ((!n || !ascii_word(text[n - 1])) && (n + len == total || !ascii_word(text[n + len])) &&
        equal_ci(std::string_view(text).substr(n, len), needle))
      return true;
  }
  return false;
}

void pgs_subtitle_info_from_metadata(struct pgs_subtitle_info* i,
                                     const char* lang,
                                     const char* title,
                                     bool forced)
{
  *i = {};
  i->active = true;
  i->forced = forced;
  // Track titles often abbreviate the script and language pair (e.g.
  // 简英双字特效 / 繁英雙字特效). Read this before metadata-only
  // completion; generic "双语" does not establish English as the second language.
  bool sim_en = has_ci(title, "简英") || has_ci(title, "簡英");
  bool tra_en = has_ci(title, "繁英");
  i->bilingual = sim_en || tra_en || has_ci(title, "中英");
  bool hans = has_ci(lang, "hans"), hant = has_ci(lang, "hant");
  bool explicit_script = hans || hant;
  bool title_sim = sim_en || has_ci(title, "简体") || has_ci(title, "簡體") ||
                   has_ci(title, "simplified") || token_ci(title, "chs");
  bool title_tra = tra_en || has_ci(title, "繁体") || has_ci(title, "繁體") ||
                   has_ci(title, "traditional") || token_ci(title, "cht");
  bool sim =
      hans || title_sim || (!explicit_script && (has_ci(lang, "zh-cn") || has_ci(lang, "zh-sg")));
  bool tra =
      hant || title_tra || (!explicit_script && (has_ci(lang, "zh-tw") || has_ci(lang, "zh-hk")));
  i->script = sim && tra ? PGS_SUB_SCRIPT_UNKNOWN
              : sim      ? PGS_SUB_SCRIPT_SIMPLIFIED
              : tra      ? PGS_SUB_SCRIPT_TRADITIONAL
                         : PGS_SUB_SCRIPT_UNKNOWN;
  i->script_from_title = sim != tra;
  // A useful explicit track name is authoritative. Keep it verbatim and
  // do not spend OCR work guessing additional traits that it does not claim.
  // A language tag alone or a generic "Chinese" title is not such a name.
  i->descriptive_title = i->script_from_title && (title_sim || title_tra || i->bilingual);
  i->forced |=
      (token_ci(title, "forced") && !has_ci(title, "non-forced") && !has_ci(title, "not forced")) ||
      has_ci(title, "强制") || has_ci(title, "強制");
  i->signs_from_title = token_ci(title, "signs") || has_ci(title, "局部翻译") ||
                        has_ci(title, "局部翻譯") || has_ci(title, "标牌") || has_ci(title, "標牌");
}

const char* pgs_sub_script_name(enum pgs_sub_script s)
{
  switch (s)
  {
    case PGS_SUB_SCRIPT_SIMPLIFIED:
      return "simplified";
    case PGS_SUB_SCRIPT_TRADITIONAL:
      return "traditional";
    case PGS_SUB_SCRIPT_MIXED:
      return "mixed";
    default:
      return "unknown";
  }
}

// A Latin word alone (a name, logo or OCR noise) is not an English caption.
int pgs_sub_word_language(const char* text, struct pgs_sub_caption_language* caption)
{
  int han = 0, latin = 0;
  std::string_view s(text);
  while (!s.empty())
  {
    int cp = next_utf8(s);
    if (cp < 0)
    {
      continue;
    }
    han += (cp >= 0x3400 && cp <= 0x9fff) || (cp >= 0x20000 && cp <= 0x2fa1f);
    latin += (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z');
  }
  if (han && !latin)
  {
    caption->han_chars += han;
    return 0;
  }
  if (!han && latin >= 2)
  {
    caption->latin_letters += latin;
    caption->latin_words++;
    return 1;
  }
  return -1;
}

void pgs_sub_line_language(const char* text, int line, int minimumConfidence,
                           struct pgs_sub_caption_language* caption)
{
  if (line != 1 || minimumConfidence < 70)
    return;
  int letters = 0;
  std::string_view s(text);
  while (!s.empty())
  {
    const int cp = next_utf8(s);
    if ((cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z'))
      ++letters;
    else if (!((cp >= 0x20 && cp <= 0x40) || (cp >= 0x5b && cp <= 0x60) ||
               (cp >= 0x7b && cp <= 0x7e) || (cp >= 0x2010 && cp <= 0x2026)))
      return; // Includes Han, other scripts and invalid UTF-8.
  }
  // A single letter is too little evidence; short words such as "No!" count.
  caption->english_second_line |= letters >= 2;
}

int pgs_sub_candidate_language(const std::string& language, const std::string& title)
{
  std::string primary = language.substr(0, language.find_first_of("-_"));
  for (char& c : primary)
    if (c >= 'A' && c <= 'Z')
      c += 'a' - 'A';
  constexpr const char* chinese[] = {"zh",  "chi", "zho", "cmn", "yue",
                                     "wuu", "nan", "hak", "hsn", "gan"};
  for (const auto* code : chinese)
    if (primary == code)
      return 1;
  if (title.find("中文") != std::string::npos || title.find("中字") != std::string::npos)
    return 1;
  if (primary.empty() || primary == "und" || primary == "unk" || primary == "unknown")
    return 2;
  return 0;
}

bool pgs_sub_matches_preference(const pgs_subtitle_info& info, int preference)
{
  if (!info.active || preference < 0 || preference > 3)
    return false;
  const auto script = preference % 2 == 0 ? PGS_SUB_SCRIPT_SIMPLIFIED : PGS_SUB_SCRIPT_TRADITIONAL;
  return info.script == script && (preference >= 2 ? info.bilingual :
      (!info.bilingual && (info.descriptive_title || info.sampled)));
}

int pgs_sub_select_preference(const std::vector<pgs_subtitle_info>& candidates,
                              int preference, int current)
{
  if (preference < 0 || preference > 3) return -1;
  const auto find = [&](int type)
  {
    if (current >= 0 && current < static_cast<int>(candidates.size()) &&
        pgs_sub_matches_preference(candidates[current], type))
      return current;
    for (size_t i = 0; i < candidates.size(); ++i)
      if (pgs_sub_matches_preference(candidates[i], type)) return static_cast<int>(i);
    return -1;
  };
  const int exact = find(preference);
  if (exact >= 0) return exact;
  const auto script = preference % 2 == 0 ? PGS_SUB_SCRIPT_SIMPLIFIED : PGS_SUB_SCRIPT_TRADITIONAL;
  for (const auto& info : candidates)
  {
    if (!info.active || info.sampled || info.descriptive_title) continue;
    if (info.script != PGS_SUB_SCRIPT_UNKNOWN && info.script != script) continue;
    // Confirmed English cannot become a monolingual candidate later.
    if (preference < 2 && info.bilingual) continue;
    return -1;
  }
  return find(preference ^ 2); // Simplified <-> Simplified/English; same for Traditional.
}

int pgs_sub_priority(const pgs_subtitle_info& info, bool chinese, bool multiline, int preference)
{
  const auto wanted = preference % 2 == 0 ? PGS_SUB_SCRIPT_SIMPLIFIED : PGS_SUB_SCRIPT_TRADITIONAL;
  const int layout = (preference >= 2 ? !multiline : multiline) ? 8 : 0;
  const int script = info.script == wanted ? 0 : info.script == PGS_SUB_SCRIPT_UNKNOWN ? 2 : 4;
  return layout + script + (chinese ? 0 : 1);
}

void pgs_sub_bilingual_feed(struct pgs_sub_bilingual_evidence* e,
                            const struct pgs_sub_caption_language* c,
                            struct pgs_subtitle_info* info)
{
  e->english_line |= c->english_second_line;
  e->chinese |= c->han_chars >= 2;
  info->bilingual |= e->english_line && (e->chinese || info->script_from_title ||
                                       info->script != PGS_SUB_SCRIPT_UNKNOWN);
  if (c->han_chars < 4 || c->latin_letters < 8 || c->latin_words < 2)
    return;
  e->bilingual++;
  info->bilingual |= e->bilingual >= 3;
}

// A useful explicit description is preserved verbatim, as in mpv.
bool pgs_metadata_describes(const char* lang, const char* title)
{
  pgs_subtitle_info info{};
  pgs_subtitle_info_from_metadata(&info, lang, title, false);
  return info.descriptive_title;
}

std::string pgs_subtitle_info_label(const pgs_subtitle_info& info,
                                    std::string (*translate)(uint32_t))
{
  uint32_t script = info.script == PGS_SUB_SCRIPT_SIMPLIFIED    ? 44400
                    : info.script == PGS_SUB_SCRIPT_TRADITIONAL ? 44401
                    : info.script == PGS_SUB_SCRIPT_MIXED       ? 44407
                                                                : 44408;
  if (info.bilingual)
  {
    script = info.script == PGS_SUB_SCRIPT_SIMPLIFIED    ? 44402
             : info.script == PGS_SUB_SCRIPT_TRADITIONAL ? 44403
                                                         : 44404;
  }
  std::string label = translate(script);
  if (info.signs_from_title)
    label += " · " + translate(44412);
  if (info.forced)
    label += " · " + translate(44413);
  return label;
}

// Continue inspecting forced-object PCS bits after expensive analysis settles.
// This does not change the container's forced disposition or track selection.
bool pgs_packet_has_forced(const uint8_t* data, size_t size)
{
  size_t pos = 0;
  while (size - pos >= 3)
  {
    const int type = data[pos];
    const size_t len = (static_cast<size_t>(data[pos + 1]) << 8) | data[pos + 2];
    pos += 3;
    if (len > size - pos)
      break;
    const uint8_t* s = data + pos;
    if (type == 0x16 && len >= 11)
    {
      size_t n = 11;
      for (int i = 0; i < s[10] && len - n >= 8; ++i)
      {
        const int flags = s[n + 3];
        if (flags & 0x40)
          return true;
        const size_t bytes = flags & 0x80 ? 16 : 8;
        if (bytes > len - n)
          break;
        n += bytes;
      }
    }
    pos += len;
  }
  return false;
}

bool pgs_packet_has_objects(const uint8_t* data, size_t size)
{
  size_t pos = 0;
  while (size - pos >= 3)
  {
    const int type = data[pos];
    const size_t length = (static_cast<size_t>(data[pos + 1]) << 8) | data[pos + 2];
    pos += 3;
    if (length > size - pos)
      return false;
    if (type == 0x16 && length >= 11)
    {
      const unsigned count = data[pos + 10];
      if (count && length >= 11 + count * 8)
        return true;
    }
    pos += length;
  }
  return false;
}
