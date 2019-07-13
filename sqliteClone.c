// building a basic sqlite clone in C 
#include<stdbool.h>
#include<stdlib.h>
#include<stdio.h>
#include<string.h>
#include<stdint.h>
#include<errno.h>
#include<fcntl.h>
#include<unistd.h> 

// many of these libraries are new to me... what can I find out about them? 

// ENUMERATED TYPES ============================================================

typedef enum {
	META_COMMAND_SUCCESS, 
	META_COMMAND_UNRECOGNIZED
} MetaCommandResult; 

typedef enum {
	PREPARE_SUCCESS,
	PREPARE_NEGATIVE_ID,
	PREPARE_STRING_TOO_LONG,
	PREPARE_SYNTAX_ERROR, 
	PREPARE_UNRECOGNIZED_STATEMENT
} PrepareResult; 

typedef enum {
	STATEMENT_INSERT, 
	STATEMENT_SELECT
} StatementType; 

typedef enum {
	EXECUTE_SUCCESS, 
	EXECUTE_TABLE_FULL
} ExecuteResult;


// TABLE AND DATA ATTRIBUTES ===================================================

#define COLUMN_USERNAME_SIZE 32
#define COLUMN_EMAIL_SIZE		 255
#define TABLE_MAX_PAGES			 100 
#define size_of_attribute(Struct, Attribute) sizeof(((Struct*)0)->Attribute)

typedef struct {
	uint32_t id; 
	char username[COLUMN_USERNAME_SIZE + 1]; 
	char email[COLUMN_EMAIL_SIZE + 1];
} Row; 

const uint32_t ID_SIZE = size_of_attribute(Row, id); 
const uint32_t USERNAME_SIZE = size_of_attribute(Row, username); 
const uint32_t EMAIL_SIZE = size_of_attribute(Row, email); 
const uint32_t ID_OFFSET = 0; 
const uint32_t USERNAME_OFFSET = ID_OFFSET + ID_SIZE; 
const uint32_t EMAIL_OFFSET = USERNAME_OFFSET + USERNAME_SIZE; 
const uint32_t ROW_SIZE = ID_SIZE + USERNAME_SIZE + EMAIL_SIZE; 

const uint32_t PAGE_SIZE = 4096; 

/* [***] depreciated after B-tree conversion [***]
const uint32_t ROWS_PER_PAGE = PAGE_SIZE / ROW_SIZE; 
const uint32_t TABLE_MAX_ROWS = ROWS_PER_PAGE * TABLE_MAX_PAGES; */

// STRUCTURE TYPES =============================================================

// InputBuffer structure definition 
// wrapper struct to interact with CLI input 
typedef struct {
	char* buffer; 
	size_t buffer_length; 
	ssize_t input_length; 
} InputBuffer; 

InputBuffer* new_input_buffer(void) {
	InputBuffer* input_buffer = malloc(sizeof(InputBuffer)); 
	input_buffer->buffer = NULL;
	input_buffer->buffer_length = 0; 
	input_buffer->input_length = 0; 
	return input_buffer;
}

// B-TREE CONVERSION ===========================================================

typedef enum {NODE_INTERNAL, NODE_LEAF} NodeType; 

	// common node header (metadata) layout 
const uint32_t NODE_TYPE_SIZE = sizeof(uint8_t); 
const uint32_t NODE_TYPE_OFFSET = 0; 
const uint32_t IS_ROOT_SIZE = sizeof(uint8_t); 
const uint32_t IS_ROOT_OFFSET = NODE_TYPE_SIZE; 
const uint32_t PARENT_POINTER_SIZE = sizeof(uint32_t); 
const uint32_t PARENT_POINTER_OFFSET = IS_ROOT_OFFSET + IS_ROOT_SIZE; 
const uint32_t COMMON_NODE_HEADER_SIZE = 
	NODE_TYPE_SIZE + IS_ROOT_SIZE + PARENT_POINTER_SIZE; 

	// leaf node header layout 
const uint32_t LEAF_NODE_NUM_CELLS_SIZE = sizeof(uint32_t); 
const uint32_t LEAF_NODE_NUM_CELLS_OFFSET = COMMON_NODE_HEADER_SIZE; 
const uint32_t LEAF_NODE_HEADER_SIZE = 
	COMMON_NODE_HEADER_SIZE + LEAF_NODE_NUM_CELLS_SIZE; 

	// leaf node body layout 
const uint32_t LEAF_NODE_KEY_SIZE = sizeof(uint32_t); 
const uint32_t LEAF_NODE_KEY_OFFSET = 0; 
const uint32_t LEAF_NODE_VALUE_SIZE = ROW_SIZE; 
const uint32_t LEAF_NODE_VALUE_OFFSET = 
	LEAF_NODE_KEY_OFFSET + LEAF_NODE_KEY_SIZE; 
const uint32_t LEAF_NODE_CELL_SIZE = LEAF_NODE_KEY_SIZE + LEAF_NODE_VALUE_SIZE; 
const uint32_t LEAF_NODE_SPACE_FOR_CELLS = PAGE_SIZE - LEAF_NODE_HEADER_SIZE; 
const uint32_t LEAF_NODE_MAX_CELLS = 
	LEAF_NODE_SPACE_FOR_CELLS / LEAF_NODE_CELL_SIZE; 

	// encoding to acces keys, values, and metadata 
	// this is all pointer arithmetic here 
uint32_t* leaf_node_num_cells(void* node) {
	return node + LEAF_NODE_NUM_CELLS_OFFSET; 
}

void* leaf_node_cell(void* node, uint32_t cell_num) {
	return node + LEAF_NODE_HEADER_SIZE + cell_num * LEAF_NODE_CELL_SIZE; 
}

uint32_t* leaf_node_key(void* node, uint32_t cell_num) {
	return leaf_node_cell(node, cell_num);
}

void* leaf_node_value(void* node, uint32_t cell_num) {
	return leaf_node_cell(node, cell_num) + LEAF_NODE_KEY_SIZE; 
}

void initialize_leaf_node(void* node) { *leaf_node_num_cells(node) = 0; }


// PAGER =======================================================================
// the pager is responsible for negotiating data persistence and 
// local session caches 
typedef struct {
	int file_descriptor;
	uint32_t file_length; 
	uint32_t num_pages; 
	void* pages[TABLE_MAX_PAGES];
} Pager; 

Pager* pager_open(const char* filename) {
	int fd = open(filename, O_RDWR | O_CREAT, S_IWUSR | S_IRUSR); 
	// translation: open file in read write mode, create file if it doesn't 
	//							exist, and set user read-write permissions

	if (fd == -1) { // an error occured 
		printf("Unable to open file %s\n", filename); 
		exit(EXIT_FAILURE); 
	}

	off_t file_length = lseek(fd, 0, SEEK_END); 
	Pager* pager = malloc(sizeof(Pager)); 
	pager->file_descriptor = fd; 
	pager->file_length = file_length; 
	pager->num_pages = (file_length / PAGE_SIZE); 

	if (file_length % PAGE_SIZE != 0) {
		printf("Db file is not a whole number of pages. Corrupt file.\n"); 
		exit(EXIT_FAILURE);
	}

	// tabula rasa 
	for (uint32_t i = 0; i < TABLE_MAX_PAGES; i++) {
		pager->pages[i] = NULL; 
	}

	return pager; 
}

void* get_page(Pager* pager, uint32_t page_num) {
	// determine if page is in bounds 
	if (page_num > TABLE_MAX_PAGES) {
		printf("Attempted fetch on out-of-bounds page number. %d > %d\n", 
			page_num, TABLE_MAX_PAGES); 
		exit(EXIT_FAILURE); 
	}
	// determine if page is in cache 
	if (pager->pages[page_num] == NULL) {
		// miss. allocate memory and load from file 
		void* page = malloc(PAGE_SIZE); 
		uint32_t num_pages = pager->file_length / PAGE_SIZE; 
		// account for any partial pages 
		if (pager->file_length % PAGE_SIZE) {
			num_pages++; 
		}

		if (page_num <= num_pages) {// should be in the cache
			lseek(pager->file_descriptor, page_num * PAGE_SIZE, SEEK_SET); 
			ssize_t bytes_read = read(pager->file_descriptor, page, PAGE_SIZE); 
			if (bytes_read == -1) {
				printf("Error reading file: %d\n", errno); 
				exit(EXIT_FAILURE); 
			}
		}
		pager->pages[page_num] = page; 

		if (page_num >= pager->num_pages) {
			pager->num_pages = page_num + 1; 
		}

	}
	return pager->pages[page_num]; 
}

void pager_flush(Pager* pager, uint32_t page_num) {
	if (pager->pages[page_num] == NULL) {
		printf("Fatal: Tried to flush NULL page\n"); 
		exit(EXIT_FAILURE); 
	}

	off_t offset = lseek(pager->file_descriptor, page_num * PAGE_SIZE, SEEK_SET); 

	if (offset == -1) {
		printf("Fatal: Error seeking: %d\n", errno); 
		exit(EXIT_FAILURE); 
	}

	ssize_t bytes_written = 
		write(pager->file_descriptor, pager->pages[page_num], PAGE_SIZE); 
	if (bytes_written == -1) {
		printf("Fatal: Write error: %d\n", errno); 
		exit(EXIT_FAILURE); 
	}
}

// =============================================================================

typedef struct {
	uint32_t num_rows;
	uint32_t root_page_num;  
	Pager* pager; 	
} Table; 

// constructor for the table structure 
Table* db_open(const char* filename) {
	// get the pager up and running 
	Pager* pager = pager_open(filename);
	
	// prepare a table 
	Table* table = malloc(sizeof(Table)); 
	table->pager = pager;
	table->root_page_num = 0; 
	
	if (pager->num_pages == 0) {
		// new database file, initialize page 0 as the leaf node 
		void* root_node = get_page(pager, 0); 
		initialize_leaf_node(root_node); 
	}
	return table; 
}

void db_close(Table* table) {
	Pager* pager = table->pager; 
	
	for (uint32_t i = 0; i < pager->num_pages; i++) {
		if (pager->pages[i] == NULL) {
			continue; 
		}
		// write to file, free page 
		pager_flush(pager, i); 
		free(pager->pages[i]); 
		pager->pages[i] = NULL; 
	}

	/* DEPRECIATED AFTER B-TREE CONVERSION 
	// we may run into a partial page write until a B-tree is set up 
	uint32_t num_additional_rows = table->num_rows % ROWS_PER_PAGE; 
	if (num_additional_rows > 0) {
		uint32_t page_num = num_full_pages; 
		if (pager->pages[page_num] != NULL) {
			pager_flush(pager, page_num, num_additional_rows * ROW_SIZE); 
			free(pager->pages[page_num]); 
			pager->pages[page_num] = NULL; 
		}
	} */ 

	int state = close(pager->file_descriptor); 
	if (state == -1) {
		printf("Fatal: Error closing db file.\n"); 
		exit(EXIT_FAILURE); 
	}
	// free all pages, pager, and table 
	for (uint32_t i = 0; i < TABLE_MAX_PAGES; i++) {
		void* page = pager->pages[i]; 
		if (page) {
			free(page); 
			pager->pages[i] = NULL; 
		}
	}
	free(pager); 
	free(table); 
}

typedef struct {
	StatementType type; 
	Row row_to_insert;		// used by INSERT statment 
} Statement; 

// CURSOR ABSTRACTION ==========================================================

typedef struct {
	Table* table; 
	uint32_t page_num;
	uint32_t cell_num; 
	bool end_of_table; 	// the cursor keeps track of the table that it's a part 
} Cursor;							// of, to make cursor function calls easier 

Cursor* table_start(Table* table) {
	Cursor* cursor = malloc(sizeof(Cursor)); 
	cursor->table = table; 
	cursor->page_num = table->root_page_num;
	cursor->cell_num = 0; 
	// orients the cursor within the tree 
	// positions are identified by the page number of the node, 
	// and the cell number within that node 
	void* root_node = get_page(table->pager, table->root_page_num); 
	uint32_t num_cells = *leaf_node_num_cells(root_node); 
	cursor->end_of_table = (num_cells == 0); 

	return cursor; 
}

Cursor* table_end(Table* table) {
	Cursor* cursor = malloc(sizeof(Cursor)); 
	cursor->table = table; 
	cursor->page_num = table->root_page_num; 

	void* root_node = get_page(table->pager, table->root_page_num); 
	uint32_t num_cells = *leaf_node_num_cells(root_node);  
	cursor->end_of_table = true; 
	
	return cursor; 
}

void cursor_advance(Cursor* cursor) {
	uint32_t page_num = cursor->page_num; 
	void* node = get_page(cursor->table->pager, page_num);

	cursor->cell_num += 1; 
	if (cursor->cell_num >= (*leaf_node_num_cells(node))) {
		// end of table reached 
		cursor->end_of_table = true; 
	}
}

void* cursor_value(Cursor* cursor) {
	uint32_t page_num = cursor->page_num;  
	void* page = get_page(cursor->table->pager, page_num); 
	return leaf_node_value(page, cursor->cell_num); 
}



// SERIALIZED ROW HELPERS ======================================================

void serialize_row(Row* source, void* destination) {
	// use of strncpy ensures the initialization of all bytes. small fix 
	memcpy(destination + ID_OFFSET, &(source->id), ID_SIZE); 
	strncpy(destination + USERNAME_OFFSET, &(source->username), USERNAME_SIZE);
	strncpy(destination + EMAIL_OFFSET, &(source->email), EMAIL_SIZE);  
}

void deserialize_row(void* source, Row* destination) {
	memcpy(&(destination->id), source + ID_OFFSET, ID_SIZE); 
	memcpy(&(destination->username), source + USERNAME_OFFSET, USERNAME_SIZE); 
	memcpy(&(destination->email), source + EMAIL_OFFSET, EMAIL_SIZE); 
}

// ROW OPERATIONS ==============================================================

void print_row(Row* row) {
	printf("(%d, %s, %s)\n", row->id, row->username, row->email); 
}

// inserts a k-v pair into leaf node, based on cursor location 
void leaf_node_insert(Cursor* cursor, uint32_t key, Row* value) {
	void* node = get_page(cursor->table->pager, cursor->page_num); 

	uint32_t num_cells = *leaf_node_num_cells(node); 
	if (num_cells >= LEAF_NODE_MAX_CELLS) {
		// node is full 
		printf("Need to implement splitting leaf node.\n");
		exit(EXIT_FAILURE); 
	}

	if (cursor->cell_num < num_cells) {
		// make room for a new cell to store serialized row 
		for (uint32_t i = num_cells; i > cursor->cell_num; i--) {
			// skates backwards from the end of the cell to the border of 
			// the most recent cell position 
			memcpy(leaf_node_cell(node, i), leaf_node_cell(node, i -1), 
				LEAF_NODE_CELL_SIZE); 
		}
	}
	// now that space has been allocated, increment the number of cells, 
	// generate a key, and insert the serialized row 
	*(leaf_node_num_cells(node)) += 1; 
	*(leaf_node_key(node, cursor->cell_num)) = key; 
	serialize_row(value, leaf_node_value(node, cursor->cell_num)); 
}

// META COMMAND SUBROUTINES ====================================================

void print_constants() {
	printf("ROW_SIZE: %d\n", ROW_SIZE); 
	printf("COMMON_NODE_HEADER_SIZE: %d\n", COMMON_NODE_HEADER_SIZE); 
	printf("LEAF_NODE_HEADER_SIZE: %d\n", LEAF_NODE_HEADER_SIZE); 
	printf("LEAF_NODE_CELL_SIZE: %d\n", LEAF_NODE_CELL_SIZE); 
	printf("LEAF_NODE_SPACE_FOR_CELLS: %d\n", LEAF_NODE_SPACE_FOR_CELLS); 
	printf("LEAF_NODE_MAX_CELLS: %d\n", LEAF_NODE_MAX_CELLS); 
}

void print_leaf_node(void* node) {
	uint32_t num_cells = *leaf_node_num_cells(node); 
	printf("leaf (size %d)\n", num_cells); 
	for (uint32_t i = 0; i < num_cells; i++) {
		uint32_t key = *leaf_node_key(node, i); 
		printf(" - %d : %d\n", i, key); 
	}
}

// FUNCTION PROTOTYPES =========================================================

void print_prompt(void); 
void read_input(InputBuffer* input_buffer); 
void close_input_buffer(InputBuffer* input_buffer); 
MetaCommandResult do_meta_command(InputBuffer* input_buffer, Table* table); 
PrepareResult prepare_insert(InputBuffer* input_buffer, Statement* statement);
PrepareResult prepare_statement(InputBuffer* input_buffer, Statement* statement);
ExecuteResult execute_insert(Statement* statement, Table* table); 
ExecuteResult execute_select(Statement* statement, Table* table);  
ExecuteResult execute_statement(Statement* statement, Table* table); 

// MAIN ========================================================================

int main(int argc, char* argv[]) {
	if (argc < 2) {
		printf("Must supply a database filename.\n"); 
		exit(EXIT_FAILURE); 
	}

	char* filename = argv[1]; 
	Table* table = db_open(filename); 

	InputBuffer* input_buffer = new_input_buffer(); 
	// REPL loop 
	while (true) {
		print_prompt(); 
		read_input(input_buffer); 
		
		if (input_buffer->buffer[0] == '.') { // meta-command cue recognized 
			
			switch (do_meta_command(input_buffer, table)) {
				case (META_COMMAND_SUCCESS): 
					continue; 
				case (META_COMMAND_UNRECOGNIZED): 
					printf("Unrecognized command %s\n", input_buffer->buffer); 
					continue; 
			}
		}
		// conversion of buffer contents to internal representation 
		Statement statement; 
		switch (prepare_statement(input_buffer, &statement)) {
			case (PREPARE_SUCCESS): 
				break; 
			case (PREPARE_NEGATIVE_ID): 
				printf("ID must be positive.\n"); 
				continue; 
			case (PREPARE_STRING_TOO_LONG): 
				printf("String is too long.\n"); 
				continue; 
			case (PREPARE_SYNTAX_ERROR): 
				printf("Syntax error. Could not parse statement.\n"); 
				continue; 
			case (PREPARE_UNRECOGNIZED_STATEMENT): 
				printf("Unrecognized keyword at start of '%s'\n", 
								input_buffer->buffer); 
				continue; 
		}

		// execute translated statement 
		switch (execute_statement(&statement, table)) {
			case (EXECUTE_SUCCESS):
				printf("Executed.\n"); 
				break; 
			case (EXECUTE_TABLE_FULL): 
				printf("Table Error: Table full.\n"); 
				break;
		}

	}
}

// =============================================================================

// print_prompt() 
// prompts user input at CLI 
void print_prompt(void) {
	printf("db > "); 
}

// read_input() 
// reads user input, storing contents in buffer 
void read_input(InputBuffer* input_buffer) {
	ssize_t bytes_read = 
	getline(&(input_buffer->buffer), &(input_buffer->buffer_length), stdin);

	if (bytes_read <= 0) {
		printf("Error reading input\n"); 
		exit(EXIT_FAILURE); 
	}

	// chop off trailing carriage return 
	input_buffer->input_length = bytes_read -1; 
	input_buffer->buffer[bytes_read - 1] = 0; 
}

// close_input_buffer() 
// responsible for freeing buffer memory 
void close_input_buffer(InputBuffer* input_buffer) {
	free(input_buffer->buffer); 
	free(input_buffer); 
	input_buffer->buffer = NULL;
	input_buffer = NULL;
}

MetaCommandResult do_meta_command(InputBuffer* input_buffer, Table* table) {
	if (strcmp(input_buffer->buffer, ".exit") == 0) {
		db_close(table); 
		exit(EXIT_SUCCESS); 
	} else if (strcmp(input_buffer->buffer, ".constants") == 0) {
		printf("Constants:\n"); 
		print_constants(); 
		return META_COMMAND_SUCCESS;
	} else if (strcmp(input_buffer->buffer, ".btree") == 0) {
		printf("Tree:\n"); 
		print_leaf_node(get_page(table->pager, 0)); 
		return META_COMMAND_SUCCESS; 
	} else {
		return META_COMMAND_UNRECOGNIZED; 
	}
}

// updated parser function to check input 
PrepareResult prepare_insert(InputBuffer* input_buffer, Statement* statement) {
	statement->type = STATEMENT_INSERT;	

	char* keyword = strtok(input_buffer->buffer, " "); 
	char* id_string = strtok(NULL, " "); 
	char* username = strtok(NULL, " "); 
	char* email 	= strtok(NULL, " "); 

	if (id_string == NULL || username == NULL || email == NULL) {
		return PREPARE_SYNTAX_ERROR;
	}

	int id = atoi(id_string);
	if (id < 0) {
		return PREPARE_NEGATIVE_ID;
	} 
	if (strlen(username) > COLUMN_USERNAME_SIZE) {
		return PREPARE_STRING_TOO_LONG;
	}
	if (strlen(email) > COLUMN_EMAIL_SIZE) {
		return PREPARE_STRING_TOO_LONG;
	}

	statement->row_to_insert.id = id; 
	strcpy(statement->row_to_insert.username, username);
	strcpy(statement->row_to_insert.email, email);

	return PREPARE_SUCCESS;
}

PrepareResult prepare_statement(InputBuffer* input_buffer, Statement* statement) {

	if (strncmp(input_buffer->buffer, "insert", 6) == 0) {
		return prepare_insert(input_buffer, statement); 
	}
	
	if (strcmp(input_buffer->buffer, "select") == 0) {
		statement->type = STATEMENT_SELECT; 
		return PREPARE_SUCCESS; 
	}
	return PREPARE_UNRECOGNIZED_STATEMENT; 
}


ExecuteResult execute_insert(Statement* statement, Table* table) {
	// verify that there's space available 
	void* node = get_page(table->pager, table->root_page_num); 
	if ((*leaf_node_num_cells(node)) >= LEAF_NODE_MAX_CELLS) {
		return EXECUTE_TABLE_FULL; 
	}
	Row* row_to_insert = &(statement->row_to_insert); 
	Cursor* cursor = table_end(table); // returns cursor to table end 
	
	// insert into leaf node 
	leaf_node_insert(cursor, row_to_insert->id, row_to_insert); 

	free(cursor); 

	return EXECUTE_SUCCESS; 
}

ExecuteResult execute_select(Statement* statement, Table* table) {
	Cursor* cursor = table_start(table);
	Row row; 

	while(!(cursor->end_of_table)) {
		deserialize_row(cursor_value(cursor), &row); 
		print_row(&row); 
		cursor_advance(cursor); 
	}

	return EXECUTE_SUCCESS;
}

ExecuteResult execute_statement(Statement* statement, Table* table) {
	switch (statement->type) {
		case (STATEMENT_INSERT): 
			return execute_insert(statement, table); 
			
		case (STATEMENT_SELECT): 
			return execute_select(statement, table); 
	}
}
