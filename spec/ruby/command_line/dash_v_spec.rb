require_relative '../spec_helper'
require_relative 'shared/verbose'

describe "The -v command line option" do
  it_behaves_like :command_line_verbose, "-v"

  describe "when used alone" do
    it "prints version and ends" do
      ruby_exe(nil, args: '-v').should include(RUBY_DESCRIPTION)
    end unless (defined?(RubyVM::YJIT) && RubyVM::YJIT.enabled?) ||
               (defined?(RubyVM::RJIT) && RubyVM::RJIT.enabled?)
  end
end
