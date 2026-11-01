#include <unity.h>

#include "logic.h"

static Sensor table[MAX_SENSORS];

void setUp() {
  for (Sensor &s : table)
    s = Sensor();
}
void tearDown() {}

static SensorMessageResult msg(const char *topic, const char *body,
                               uint32_t now = 0) {
  return applySensorMessage(table, MAX_SENSORS, topic, body, now);
}

static int used() {
  int n = 0;
  for (const Sensor &s : table)
    n += s.used;
  return n;
}

static void getPage(const PageOptions &o, uint32_t now, int idx, char *out,
                    size_t len) {
  bool isName;
  collectPages(table, MAX_SENSORS, o, now, idx, out, len, isName);
}

static int pageCount(const PageOptions &o, uint32_t now = 0) {
  bool isName;
  return collectPages(table, MAX_SENSORS, o, now, -1, nullptr, 0, isName);
}

void test_temperature_conversion() {
  TEST_ASSERT_FLOAT_WITHIN(0.001, 20.0, convertTemp(20.0f, 'C'));
  TEST_ASSERT_FLOAT_WITHIN(0.001, 68.0, convertTemp(20.0f, 'F'));
  TEST_ASSERT_FLOAT_WITHIN(0.001, 293.15, convertTemp(20.0f, 'K'));
  TEST_ASSERT_FLOAT_WITHIN(0.01, 527.67, convertTemp(20.0f, 'R'));
  TEST_ASSERT_EQUAL_STRING("K", tempSuffix('K'));
  TEST_ASSERT_EQUAL_STRING("^F", tempSuffix('F'));
}

void test_plato() {
  TEST_ASSERT_FLOAT_WITHIN(0.1, 0.0, sgToPlato(1.000f));
  TEST_ASSERT_FLOAT_WITHIN(0.1, 12.0, sgToPlato(1.048f));
}

void test_stale_survives_millis_rollover() {
  Sensor s;
  s.lastUpdate = 0xFFFFFF00u;
  TEST_ASSERT_FALSE(isStale(s, 0x00000100u, 30));
  TEST_ASSERT_TRUE(isStale(s, (uint32_t)(s.lastUpdate + 31UL * 60000UL), 30));
  TEST_ASSERT_FALSE(
      isExpired(s, (uint32_t)(s.lastUpdate + 30UL * 60000UL), 60));
}

void test_message_rounding_and_fields() {
  TEST_ASSERT_EQUAL(MSG_UPDATED,
                    msg("fermwatch/tilt_yellow",
                        "{\"sensor\":\"Tilt Yellow\",\"sg\":1.01234,"
                        "\"temperature\":20.46,\"trend\":-0.004249}"));
  TEST_ASSERT_EQUAL(1, used());
  TEST_ASSERT_EQUAL_STRING("Tilt Yellow", table[0].name);
  TEST_ASSERT_EQUAL_STRING("tilt_yellow", table[0].key);
  TEST_ASSERT_FLOAT_WITHIN(1e-6, 1.012, table[0].sg);
  TEST_ASSERT_FLOAT_WITHIN(1e-6, 20.5, table[0].temp);
  TEST_ASSERT_FLOAT_WITHIN(1e-6, -0.0042, table[0].trend);
}

void test_numeric_strings_and_unavailable() {
  msg("fermwatch/a", "{\"sensor\":\"A\",\"sg\":\"1.050\",\"temperature\":20}");
  TEST_ASSERT_FLOAT_WITHIN(1e-6, 1.05, table[0].sg);
  msg("fermwatch/a", "{\"sensor\":\"A\",\"sg\":\"unavailable\",\"temperature\":"
                     "\"unknown\",\"trend\":0.01}");
  TEST_ASSERT_FLOAT_WITHIN(1e-6, 1.05, table[0].sg);
  TEST_ASSERT_FLOAT_WITHIN(1e-6, 20.0, table[0].temp);
  TEST_ASSERT_FLOAT_WITHIN(1e-6, 0.01, table[0].trend);
}

void test_invalid_messages_ignored() {
  TEST_ASSERT_EQUAL(MSG_IGNORED, msg("fermwatch/a", "not json"));
  TEST_ASSERT_EQUAL(MSG_IGNORED, msg("fermwatch/a", "{\"sg\":1.01}"));
  TEST_ASSERT_EQUAL(MSG_IGNORED, msg("fermwatch/a", "{\"sensor\":\"A\"}"));
  TEST_ASSERT_EQUAL(MSG_IGNORED, msg("fermwatch/a", "[1,2]"));
  TEST_ASSERT_EQUAL(0, used());
}

void test_same_name_case_insensitive() {
  msg("fermwatch/a", "{\"sensor\":\"Tilt\",\"sg\":1.01}");
  msg("fermwatch/b", "{\"sensor\":\"TILT\",\"temperature\":18}");
  TEST_ASSERT_EQUAL(1, used());
  TEST_ASSERT_FLOAT_WITHIN(1e-6, 1.01, table[0].sg);
  TEST_ASSERT_FLOAT_WITHIN(1e-6, 18.0, table[0].temp);
}

void test_empty_payload_removes_by_key_or_name() {
  msg("fermwatch/tilt_red", "{\"sensor\":\"Tilt Red\",\"sg\":1.01}");
  msg("fermwatch/x", "{\"sensor\":\"Other\",\"sg\":1.02}");
  TEST_ASSERT_EQUAL(MSG_REMOVED, msg("fermwatch/tilt_red", ""));
  TEST_ASSERT_EQUAL(1, used());
  TEST_ASSERT_EQUAL(MSG_REMOVED, msg("fermwatch/Other", ""));
  TEST_ASSERT_EQUAL(0, used());
  TEST_ASSERT_EQUAL(MSG_IGNORED, msg("fermwatch/none", ""));
}

void test_full_table_evicts_least_recently_updated() {
  char body[64];
  for (int i = 0; i < MAX_SENSORS; i++) {
    snprintf(body, sizeof(body), "{\"sensor\":\"S%d\",\"sg\":1.01}", i);
    msg("fermwatch/s", body, 1000 + i * 10);
  }
  msg("fermwatch/s", "{\"sensor\":\"S0\",\"sg\":1.02}", 5000);
  msg("fermwatch/new", "{\"sensor\":\"New\",\"sg\":1.03}", 6000);
  TEST_ASSERT_EQUAL(MAX_SENSORS, used());
  bool hasS0 = false, hasS1 = false, hasNew = false;
  for (const Sensor &s : table) {
    hasS0 |= !strcmp(s.name, "S0");
    hasS1 |= !strcmp(s.name, "S1");
    hasNew |= !strcmp(s.name, "New");
  }
  TEST_ASSERT_TRUE(hasS0);
  TEST_ASSERT_FALSE(hasS1);
  TEST_ASSERT_TRUE(hasNew);
}

void test_pages_default() {
  msg("fermwatch/a",
      "{\"sensor\":\"Tilt Yellow\",\"sg\":1.012,\"temperature\":20.4,"
      "\"trend\":-0.0042}");
  PageOptions o;
  char p[64];
  TEST_ASSERT_EQUAL(4, pageCount(o));
  getPage(o, 0, 0, p, sizeof(p));
  TEST_ASSERT_EQUAL_STRING("TILT YELLOW", p);
  getPage(o, 0, 1, p, sizeof(p));
  TEST_ASSERT_EQUAL_STRING("SG 1.012", p);
  getPage(o, 0, 2, p, sizeof(p));
  TEST_ASSERT_EQUAL_STRING("T 20.4^C", p);
  getPage(o, 0, 3, p, sizeof(p));
  TEST_ASSERT_EQUAL_STRING("-0.0042/d", p);
}

void test_pages_options_and_missing_values() {
  msg("fermwatch/a", "{\"sensor\":\"A\",\"sg\":1.012}");
  PageOptions o;
  TEST_ASSERT_EQUAL(2, pageCount(o));
  o.showName = false;
  TEST_ASSERT_EQUAL(1, pageCount(o));
  o.showSg = false;
  TEST_ASSERT_EQUAL(0, pageCount(o));
}

void test_pages_stale_prefix_and_units() {
  msg("fermwatch/a",
      "{\"sensor\":\"A\",\"sg\":1.048,\"temperature\":20,\"trend\":-0.004}", 0);
  PageOptions o;
  o.tempUnit = 'F';
  char p[64];
  getPage(o, 31UL * 60000UL, 2, p, sizeof(p));
  TEST_ASSERT_EQUAL_STRING("!T 68.0^F", p);
  o.tempUnit = 'K';
  getPage(o, 0, 2, p, sizeof(p));
  TEST_ASSERT_EQUAL_STRING("T 293.1K", p);
}

void test_plato_pages() {
  msg("fermwatch/a", "{\"sensor\":\"A\",\"sg\":1.048,\"trend\":-0.004}");
  PageOptions o;
  o.plato = true;
  char p[64];
  getPage(o, 0, 1, p, sizeof(p));
  TEST_ASSERT_EQUAL_STRING("11.9^P", p);
  getPage(o, 0, 2, p, sizeof(p));
  TEST_ASSERT_NOT_NULL(strstr(p, "^P/d"));

  setUp();
  msg("fermwatch/a", "{\"sensor\":\"A\",\"trend\":-0.004}");
  TEST_ASSERT_EQUAL(1, pageCount(o)); // Plato trend needs SG: name only
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_temperature_conversion);
  RUN_TEST(test_plato);
  RUN_TEST(test_stale_survives_millis_rollover);
  RUN_TEST(test_message_rounding_and_fields);
  RUN_TEST(test_numeric_strings_and_unavailable);
  RUN_TEST(test_invalid_messages_ignored);
  RUN_TEST(test_same_name_case_insensitive);
  RUN_TEST(test_empty_payload_removes_by_key_or_name);
  RUN_TEST(test_full_table_evicts_least_recently_updated);
  RUN_TEST(test_pages_default);
  RUN_TEST(test_pages_options_and_missing_values);
  RUN_TEST(test_pages_stale_prefix_and_units);
  RUN_TEST(test_plato_pages);
  return UNITY_END();
}
