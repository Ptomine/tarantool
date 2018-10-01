#include "json/json.h"
#include "unit.h"
#include "trivia/util.h"
#include <string.h>
#include <stdbool.h>

#define reset_to_new_path(value) \
	path = value; \
	len = strlen(value); \
	json_lexer_create(&lexer, path, len);

#define is_next_index(value_len, value) \
	path = lexer.src + lexer.offset; \
	is(json_lexer_next_token(&lexer, &token), 0, "parse <%." #value_len "s>", \
	   path); \
	is(token.key.type, JSON_TOKEN_NUM, "<%." #value_len "s> is num", path); \
	is(token.key.num, value, "<%." #value_len "s> is " #value, path);

#define is_next_key(value) \
	len = strlen(value); \
	is(json_lexer_next_token(&lexer, &token), 0, "parse <" value ">"); \
	is(token.key.type, JSON_TOKEN_STR, "<" value "> is str"); \
	is(token.key.len, len, "len is %d", len); \
	is(strncmp(token.key.str, value, len), 0, "str is " value);

void
test_basic()
{
	header();
	plan(71);
	const char *path;
	int len;
	struct json_lexer lexer;
	struct json_token token;

	reset_to_new_path("[0].field1.field2['field3'][5]");
	is_next_index(3, 0);
	is_next_key("field1");
	is_next_key("field2");
	is_next_key("field3");
	is_next_index(3, 5);

	reset_to_new_path("[3].field[2].field")
	is_next_index(3, 3);
	is_next_key("field");
	is_next_index(3, 2);
	is_next_key("field");

	reset_to_new_path("[\"f1\"][\"f2'3'\"]");
	is_next_key("f1");
	is_next_key("f2'3'");

	/* Support both '.field1...' and 'field1...'. */
	reset_to_new_path(".field1");
	is_next_key("field1");
	reset_to_new_path("field1");
	is_next_key("field1");

	/* Long number. */
	reset_to_new_path("[1234]");
	is_next_index(6, 1234);

	/* Empty path. */
	reset_to_new_path("");
	is(json_lexer_next_token(&lexer, &token), 0, "parse empty path");
	is(token.key.type, JSON_TOKEN_END, "is str");

	/* Path with no '.' at the beginning. */
	reset_to_new_path("field1.field2");
	is_next_key("field1");

	/* Unicode. */
	reset_to_new_path("[2][6]['привет中国world']['中国a']");
	is_next_index(3, 2);
	is_next_index(3, 6);
	is_next_key("привет中国world");
	is_next_key("中国a");

	check_plan();
	footer();
}

#define check_new_path_on_error(value, errpos) \
	reset_to_new_path(value); \
	struct json_token token; \
	is(json_lexer_next_token(&lexer, &token), errpos, "error on position %d" \
	   " for <%s>", errpos, path);

struct path_and_errpos {
	const char *path;
	int errpos;
};

void
test_errors()
{
	header();
	plan(20);
	const char *path;
	int len;
	struct json_lexer lexer;
	const struct path_and_errpos errors[] = {
		/* Double [[. */
		{"[[", 2},
		/* Not string inside []. */
		{"[field]", 2},
		/* String outside of []. */
		{"'field1'.field2", 1},
		/* Empty brackets. */
		{"[]", 2},
		/* Empty string. */
		{"''", 1},
		/* Spaces between identifiers. */
		{" field1", 1},
		/* Start from digit. */
		{"1field", 1},
		{".1field", 2},
		/* Unfinished identifiers. */
		{"['field", 8},
		{"['field'", 9},
		{"[123", 5},
		{"['']", 3},
		/*
		 * Not trivial error: can not write
		 * '[]' after '.'.
		 */
		{".[123]", 2},
		/* Misc. */
		{"[.]", 2},
		/* Invalid UNICODE */
		{"['aaa\xc2\xc2']", 6},
		{".\xc2\xc2", 2},
	};
	for (size_t i = 0; i < lengthof(errors); ++i) {
		reset_to_new_path(errors[i].path);
		int errpos = errors[i].errpos;
		struct json_token token;
		is(json_lexer_next_token(&lexer, &token), errpos,
		   "error on position %d for <%s>", errpos, path);
	}

	reset_to_new_path("f.[2]")
	struct json_token token;
	json_lexer_next_token(&lexer, &token);
	is(json_lexer_next_token(&lexer, &token), 3, "can not write <field.[index]>")

	reset_to_new_path("f.")
	json_lexer_next_token(&lexer, &token);
	is(json_lexer_next_token(&lexer, &token), 3, "error in leading <.>");

	reset_to_new_path("fiel d1")
	json_lexer_next_token(&lexer, &token);
	is(json_lexer_next_token(&lexer, &token), 5, "space inside identifier");

	reset_to_new_path("field\t1")
	json_lexer_next_token(&lexer, &token);
	is(json_lexer_next_token(&lexer, &token), 6, "tab inside identifier");

	check_plan();
	footer();
}

struct test_struct {
	int value;
	struct json_token node;
};

struct test_struct *
test_struct_alloc(struct test_struct *records_pool, int *pool_idx)
{
	struct test_struct *ret = &records_pool[*pool_idx];
	*pool_idx = *pool_idx + 1;
	memset(&ret->node, 0, sizeof(ret->node));
	return ret;
}

struct test_struct *
test_add_path(struct json_tree *tree, const char *path, uint32_t path_len,
	      struct test_struct *records_pool, int *pool_idx)
{
	int rc;
	struct json_lexer lexer;
	struct json_token *parent = NULL;
	json_lexer_create(&lexer, path, path_len);
	struct test_struct *field = test_struct_alloc(records_pool, pool_idx);
	while ((rc = json_lexer_next_token(&lexer, &field->node)) == 0 &&
		field->node.key.type != JSON_TOKEN_END) {
		struct json_token *next =
			json_tree_lookup(tree, parent, &field->node);
		if (next == NULL) {
			rc = json_tree_add(tree, parent, &field->node);
			fail_if(rc != 0);
			next = &field->node;
			field = test_struct_alloc(records_pool, pool_idx);
		}
		parent = next;
	}
	fail_if(rc != 0 || field->node.key.type != JSON_TOKEN_END);
	*pool_idx = *pool_idx - 1;
	/* release field */
	return json_tree_entry(parent, struct test_struct, node);
}

void
test_tree()
{
	header();
	plan(35);

	struct json_tree tree;
	int rc = json_tree_create(&tree);
	fail_if(rc != 0);

	struct test_struct records[6];
	for (int i = 0; i < 6; i++)
		records[i].value = i;

	const char *path1 = "[1][10]";
	const char *path2 = "[1][20].file";
	const char *path_unregistered = "[1][3]";

	int records_idx = 1;
	struct test_struct *node;
	node = test_add_path(&tree, path1, strlen(path1), records,
			     &records_idx);
	is(node, &records[2], "add path '%s'", path1);

	node = test_add_path(&tree, path2, strlen(path2), records,
			     &records_idx);
	is(node, &records[4], "add path '%s'", path2);

	node = json_tree_lookup_path_entry(&tree, NULL, path1, strlen(path1),
					   struct test_struct, node);
	is(node, &records[2], "lookup path '%s'", path1);

	node = json_tree_lookup_path_entry(&tree, NULL, path2, strlen(path2),
					   struct test_struct, node);
	is(node, &records[4], "lookup path '%s'", path2);

	node = json_tree_lookup_path_entry(&tree, NULL, path_unregistered,
					   strlen(path_unregistered),
					   struct test_struct, node);
	is(node, NULL, "lookup unregistered path '%s'", path_unregistered);

	/* Test iterators. */
	struct json_token *token = NULL;
	const struct json_token *tokens_preorder[] =
		{&records[1].node, &records[2].node,
		 &records[3].node, &records[4].node};
	int cnt = sizeof(tokens_preorder)/sizeof(tokens_preorder[0]);
	int idx = 0;

	json_tree_foreach_preorder(token, &tree.root) {
		if (idx >= cnt)
			break;
		struct test_struct *t1 =
			json_tree_entry(token, struct test_struct, node);
		struct test_struct *t2 =
			json_tree_entry(tokens_preorder[idx],
					struct test_struct, node);
		is(token, tokens_preorder[idx],
		   "test foreach pre order %d: have %d expected of %d",
		   idx, t1->value, t2->value);
		++idx;
	}
	is(idx, cnt, "records iterated count %d of %d", idx, cnt);

	const struct json_token *tree_nodes_postorder[] =
		{&records[2].node, &records[4].node,
		 &records[3].node, &records[1].node};
	cnt = sizeof(tree_nodes_postorder)/sizeof(tree_nodes_postorder[0]);
	idx = 0;
	json_tree_foreach_postorder(token, &tree.root) {
		if (idx >= cnt)
			break;
		struct test_struct *t1 =
			json_tree_entry(token, struct test_struct, node);
		struct test_struct *t2 =
			json_tree_entry(tree_nodes_postorder[idx],
					struct test_struct, node);
		is(token, tree_nodes_postorder[idx],
		   "test foreach post order %d: have %d expected of %d",
		   idx, t1->value, t2->value);
		++idx;
	}
	is(idx, cnt, "records iterated count %d of %d", idx, cnt);

	idx = 0;
	json_tree_foreach_safe(token, &tree.root) {
		if (idx >= cnt)
			break;
		struct test_struct *t1 =
			json_tree_entry(token, struct test_struct, node);
		struct test_struct *t2 =
			json_tree_entry(tree_nodes_postorder[idx],
					struct test_struct, node);
		is(token, tree_nodes_postorder[idx],
		   "test foreach safe order %d: have %d expected of %d",
		   idx, t1->value, t2->value);
		++idx;
	}
	is(idx, cnt, "records iterated count %d of %d", idx, cnt);

	idx = 0;
	json_tree_foreach_entry_preorder(node, &tree.root, struct test_struct,
					 node) {
		if (idx >= cnt)
			break;
		struct test_struct *t =
			json_tree_entry(tokens_preorder[idx],
					struct test_struct, node);
		is(&node->node, tokens_preorder[idx],
		   "test foreach entry pre order %d: have %d expected of %d",
		   idx, node->value, t->value);
		idx++;
	}
	is(idx, cnt, "records iterated count %d of %d", idx, cnt);

	idx = 0;
	json_tree_foreach_entry_postorder(node, &tree.root, struct test_struct,
					  node) {
		if (idx >= cnt)
			break;
		struct test_struct *t =
			json_tree_entry(tree_nodes_postorder[idx],
					struct test_struct, node);
		is(&node->node, tree_nodes_postorder[idx],
		   "test foreach entry post order %d: have %d expected of %d",
		   idx, node->value, t->value);
		idx++;
	}
	is(idx, cnt, "records iterated count %d of %d", idx, cnt);

	idx = 0;
	json_tree_foreach_entry_safe(node, &tree.root, struct test_struct,
				     node) {
		if (idx >= cnt)
			break;
		struct test_struct *t =
			json_tree_entry(tree_nodes_postorder[idx],
					struct test_struct, node);
		is(&node->node, tree_nodes_postorder[idx],
		   "test foreach entry safe order %d: have %d expected of %d",
		   idx, node->value, t->value);
		json_tree_del(&tree, &node->node);
		idx++;
	}
	is(idx, cnt, "records iterated count %d of %d", idx, cnt);
	json_tree_destroy(&tree);

	check_plan();
	footer();
}

int
main()
{
	header();
	plan(3);

	test_basic();
	test_errors();
	test_tree();

	int rc = check_plan();
	footer();
	return rc;
}
