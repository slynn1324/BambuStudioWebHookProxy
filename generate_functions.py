import csv
import re

function_template = """
{return_type} (*func_{name})({args}) = NULL;

extern "C" {return_type} {name}({args}){{
	_BSWHPLOG_VERBOSE("{name}\\n");

	if ( !func_{name} ){{
		void * f = get_function("{name}");
		func_{name} = (({return_type}(*)({args}))f);
	}}

	return func_{name}({arg_names});
}}
"""


with open("sigs.csv", newline="") as csvfile:
	reader = csv.reader(csvfile, delimiter="|")
	for row in reader:
		if len(row) >= 2:
			name = row[0].strip();
			return_type = row[1].strip();
			args_str = row[2].strip();

			
			arg_names = []

			args = re.sub("<.*>", "", args_str).split(",");

			for arg in args:
				arg_name = arg.strip().split(" ")[-1].replace("*", "").replace("&", "")
				arg_names.append(arg_name)

			arg_names_str = ", ".join(arg_names)

			print(function_template.format(return_type=return_type, name=name, args=args_str, arg_names=arg_names_str));
