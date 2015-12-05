#include "client\pointers.hpp"
#include "common_helpers.hpp"

namespace intercept {
	namespace sqf {
		namespace __helpers {

            void __empty_nular(nular_function fnc_) {
                host::functions.invoke_raw_nular(fnc_);
            }

            std::string __retrieve_nular_string(nular_function fnc_) {
                game_value function_return_number = host::functions.invoke_raw_nular(fnc_);
                std::string ret_number = ((game_data_string *)function_return_number.data)->get_string();
                host::functions.free_value(&function_return_number);

                return ret_number;
            }

			float __retrieve_nular_number(nular_function fnc_) {
				game_value function_return_number = host::functions.invoke_raw_nular(fnc_);
				float ret_number = ((game_data_number *)function_return_number.data)->number;
				host::functions.free_value(&function_return_number);

				return ret_number;
			}

            object __retrieve_nular_object(nular_function fnc_) {
                game_value function_return_number = host::functions.invoke_raw_nular(fnc_);
                return std::make_shared<object_ptr>(function_return_number.data);
            }

			vector3 __retrieve_nular_vector3(nular_function fnc_) {
				game_value function_return_array = host::functions.invoke_raw_nular(fnc_);
				float x = ((game_data_number *)((game_data_array *)function_return_array.data)->data[0].data)->number;
				float y = ((game_data_number *)((game_data_array *)function_return_array.data)->data[1].data)->number;
				float z = ((game_data_number *)((game_data_array *)function_return_array.data)->data[2].data)->number;
				host::functions.free_value(&function_return_array);
				return vector3(x, y, z);
			}

			bool __retrieve_nular_bool(nular_function fnc_) {
				game_value function_return_bool = host::functions.invoke_raw_nular(fnc_);
				bool ret_bool = ((game_data_bool *)function_return_bool.data)->value;
				host::functions.free_value(&function_return_bool);
				return ret_bool;
			}


            std::vector<object>& all_objects(nular_function fnc_) {
                game_value intersects_value = host::functions.invoke_raw_nular(fnc_);
                game_data_array* intersects = ((game_data_array *)intersects_value.data);

                std::vector<object> output;
                for (int i = 0; i < intersects->length; ++i) {
                    output.push_back(std::make_shared<object_ptr>(intersects->data[i]));
                }
                host::functions.free_value(&intersects_value);
                return output;
            }

            std::vector<std::string>& _nular_strings(nular_function fnc_) {
                game_value intersects_value = host::functions.invoke_raw_nular(fnc_);
                game_data_array* intersects = ((game_data_array *)intersects_value.data);

                std::vector<std::string> output;
                for (int i = 0; i < intersects->length; ++i) {
                    std::string value = ((game_data_string *)intersects->data[i].data)->get_string();
                    output.push_back(value);
                }
                host::functions.free_value(&intersects_value); // TODO do we also have to free all the strings in the array?
                return output;
            }

            std::vector<team_member>& _nular_team_members(nular_function fnc_) {
                game_value intersects_value = host::functions.invoke_raw_nular(fnc_);
                game_data_array* intersects = ((game_data_array *)intersects_value.data);

                std::vector<team_member> output;
                for (int i = 0; i < intersects->length; ++i) {
                    output.push_back(std::make_shared<team_member_ptr>(intersects->data[i]));
                }
                host::functions.free_value(&intersects_value);
                return output;
            }

            std::vector<group>& all_groups(nular_function fnc_) {
                game_value intersects_value = host::functions.invoke_raw_nular(fnc_);
                game_data_array* intersects = ((game_data_array *)intersects_value.data);

                std::vector<group> output;
                for (int i = 0; i < intersects->length; ++i) {
                    output.push_back(std::make_shared<group_ptr>(intersects->data[i]));
                }
                host::functions.free_value(&intersects_value);
                return output;
            }

            std::vector<display>& all_displays(nular_function fnc_) {
                game_value intersects_value = host::functions.invoke_raw_nular(fnc_);
                game_data_array* intersects = ((game_data_array *)intersects_value.data);

                std::vector<display> output;
                for (int i = 0; i < intersects->length; ++i) {
                    output.push_back(std::make_shared<display_ptr>(intersects->data[i]));
                }
                host::functions.free_value(&intersects_value);
                return output;
            }
            std::vector<marker>& all_map_markers(nular_function fnc_) {
                game_value intersects_value = host::functions.invoke_raw_nular(fnc_);
                game_data_array* intersects = ((game_data_array *)intersects_value.data);

                std::vector<marker> output;
                for (int i = 0; i < intersects->length; ++i) { // TODO figure out converting markers in an array to an std vector marker 
                                                               // output.push_back((host::functions.new_string((const char *)(intersects->data[i]))));
                }
                host::functions.free_value(&intersects_value);
                return output;
            }

			void __empty_unary_obj(unary_function fnc_, object obj_)
            {
				host::functions.invoke_raw_unary(fnc_, *obj_);
            }

			void __empty_unary_vector(unary_function fnc_, vector3 vec_)
            {
				host::functions.invoke_raw_unary(fnc_, game_value_vector3(vec_));
            }

			void __empty_unary_string(unary_function fnc_, std::string str_)
            {
				host::functions.invoke_raw_unary(fnc_, game_value_string(str_));
            }

			void __empty_unary_number(unary_function fnc_, float val_)
            {
				host::functions.invoke_raw_unary(fnc_, game_value_number(val_));
            }

			void __empty_unary_bool(unary_function fnc_, bool val_)
            {
				host::functions.invoke_raw_unary(fnc_, game_value_bool(val_));
            }
		}
	}
}