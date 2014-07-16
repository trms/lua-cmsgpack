local msgpack, ret_str, test_table, ret_table, msg_start_index, new_message, msg_ud

describe ("msg_mspack operation #msg_mspack", function()
	setup(function()
		msgpack = require'cmsgpack'

		if pcall(require, "message") then
			print"using message test module"
			new_message = require'message'.message
			assert.are_equal(false, pcall(require, "nml")) -- if nml is available I want to test it
		elseif pcall(require, "nml") then
			print"using nml module"
			new_message = require'nml'.core.nml_msg
		end
		assert.is_truthy(new_message)
		test_table = {"Hello,", "world", "!"}
	end)

	after_each(function()
		msg_ud = nil
		collectgarbage()
	end)

	it("can be loaded", function()
		assert.is_truthy(msgpack)
	end)

	it("has a pack function", function()
		assert.are_equal("function", type(msgpack.pack))
	end)

	it("has an unpack function", function()
		assert.are_equal("function", type(msgpack.unpack))
	end)

	it("can make a new message with a table and return a string of the value.", function()
		ret_str = msgpack.pack(test_table)
		assert.are_equal("string", type(ret_str))
	end)

	it("given a string, it can parse a msgpack encoded message and turn it into a lua table.", function()
		ret_table = msgpack.unpack(ret_str)
		assert.are.same(test_table, ret_table)
	end)

	it("given a table and a message userdata, it can populate the message and return the same userdata object, but with the buffer filled in.", function()
		msg_ud = new_message()
		assert.is_truthy(msg_ud)
		assert.are_equal("userdata", type(msg_ud))

		-- this will call realloc on the message's metatable
		local ret_ud = msgpack.packmessage(msg_ud, test_table)
		assert.are_equal("userdata", type(ret_ud)) -- we got a msg_ud
		assert.are_equal(#msg_ud, #ret_ud) -- it used the message we passed it
		ret_ud = nil
	end)

	it("given an nml_msg, it can parse the msgpack encoded data and turn it into a lua table.", function()
		msg_ud = new_message()
		local ret_ud = msgpack.packmessage(msg_ud, test_table)
		
		ret_table = msgpack.unpackmessage(ret_ud)
		assert.are_same(test_table, ret_table)
		ret_ud = nil
	end)
	
	it("can encode a table that has a nested nml_msg userdata.", function()
		--todo
	end)
end)