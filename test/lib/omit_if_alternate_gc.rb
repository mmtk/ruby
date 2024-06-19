module OmitIfAlternateGC
  def omit_if_alternate_gc
    if defined?(GC::MMTk) && GC::MMTk.enabled?
      omit "alternative GC in use"
    end
  end
end

